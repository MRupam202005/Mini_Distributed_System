/* =============================================================================
 * machine_a.c  —  Coordinator / Dispatcher  (Machine A)
 *
 * Responsibilities:
 *   1. Compile a user-supplied .c file with gcc.
 *   2. Query all known worker nodes for their CPU load (/proc/loadavg).
 *   3. Select the worker with the lowest 1-minute load average.
 *   4. Transfer the compiled binary over TCP using the custom protocol.
 *   5. Wait for the captured stdout/stderr and print it to the terminal.
 *
 * Usage:
 *   ./machine_a <source.c> [worker1_ip] [worker2_ip] ...
 *   Example:
 *   ./machine_a task.c 192.168.1.10 192.168.1.11 192.168.1.12
 * ============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"

/* ── Types ─────────────────────────────────────────────────────────────── */

/** Holds the result of querying a single worker's load. */
typedef struct {
    char          ip[INET_ADDRSTRLEN];
    load_payload_t load;
    int            reachable;   /**< 1 = responded, 0 = timed out/error */
} worker_info_t;

/* ── Forward declarations ────────────────────────────────────────────────*/

static int  compile_source(const char *src_path, const char *out_path);
static int  connect_to_worker(const char *ip, int port);
static int  query_worker_load(const char *ip, worker_info_t *info);
static int  select_best_worker(worker_info_t *workers, int count);
static int  send_binary(int sockfd, const char *bin_path);
static int  receive_output(int sockfd);
static void set_socket_timeout(int sockfd, int seconds);
static void print_usage(const char *progname);

/* ═══════════════════════════════════════════════════════════════════════
 * main()
 * ═════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc < 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *src_path  = argv[1];
    int         nworkers  = argc - 2;
    char      **worker_ips = &argv[2];

    /* ── Validate source file ───────────────────────────────────────── */
    if (access(src_path, R_OK) != 0) {
        fprintf(stderr, "[A] ERROR: Cannot read source file '%s': %s\n",
                src_path, strerror(errno));
        return EXIT_FAILURE;
    }

    /* ── Step 1: Compile the source file ────────────────────────────── */
    char bin_path[] = "/tmp/dte_compiled_XXXXXX";
    int  tmp_fd = mkstemp(bin_path);
    if (tmp_fd < 0) {
        perror("[A] mkstemp");
        return EXIT_FAILURE;
    }
    close(tmp_fd);   /* gcc will overwrite it */

    printf("[A] Compiling '%s' → '%s' ...\n", src_path, bin_path);
    if (compile_source(src_path, bin_path) != 0) {
        fprintf(stderr, "[A] Compilation failed. Aborting.\n");
        unlink(bin_path);
        return EXIT_FAILURE;
    }
    printf("[A] Compilation successful.\n");

    /* ── Step 2: Query all workers for their load ───────────────────── */
    printf("[A] Querying %d worker(s) for CPU load ...\n", nworkers);

    worker_info_t workers[MAX_WORKERS];
    int           wcount = (nworkers < MAX_WORKERS) ? nworkers : MAX_WORKERS;

    for (int i = 0; i < wcount; i++) {
        memset(&workers[i], 0, sizeof(workers[i]));
        strncpy(workers[i].ip, worker_ips[i], INET_ADDRSTRLEN - 1);
        query_worker_load(worker_ips[i], &workers[i]);

        if (workers[i].reachable) {
            printf("  Worker %-15s  load(1m)=%.2f  load(5m)=%.2f  "
                   "load(15m)=%.2f  procs=%u/%u\n",
                   workers[i].ip,
                   workers[i].load.load_1min,
                   workers[i].load.load_5min,
                   workers[i].load.load_15min,
                   workers[i].load.running_procs,
                   workers[i].load.total_procs);
        } else {
            printf("  Worker %-15s  UNREACHABLE\n", workers[i].ip);
        }
    }

    /* ── Step 3: Select the best (lowest-load) worker ──────────────── */
    int chosen = select_best_worker(workers, wcount);
    if (chosen < 0) {
        fprintf(stderr, "[A] No reachable workers found. Aborting.\n");
        unlink(bin_path);
        return EXIT_FAILURE;
    }
    printf("[A] Selected worker: %s (load=%.2f)\n",
           workers[chosen].ip, workers[chosen].load.load_1min);

    /* ── Step 4: Connect to selected worker and transfer binary ─────── */
    printf("[A] Connecting to %s:%d ...\n", workers[chosen].ip, WORKER_PORT);
    int sockfd = connect_to_worker(workers[chosen].ip, WORKER_PORT);
    if (sockfd < 0) {
        fprintf(stderr, "[A] Connection to worker failed.\n");
        unlink(bin_path);
        return EXIT_FAILURE;
    }
    set_socket_timeout(sockfd, NETWORK_TIMEOUT_SEC);

    printf("[A] Sending binary ...\n");
    if (send_binary(sockfd, bin_path) != 0) {
        fprintf(stderr, "[A] Binary transfer failed.\n");
        close(sockfd);
        unlink(bin_path);
        return EXIT_FAILURE;
    }
    printf("[A] Binary sent. Waiting for execution output ...\n");

    /* ── Step 5: Receive and display output ─────────────────────────── */
    int ret = receive_output(sockfd);

    close(sockfd);
    unlink(bin_path);

    return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ═══════════════════════════════════════════════════════════════════════
 * compile_source()
 *
 * Invokes gcc to compile @src_path and write the ELF binary to @out_path.
 * Returns 0 on success, -1 if gcc reports an error.
 *
 * Security note: In a production system you would use execvp() + fork()
 * with a carefully constructed argv[] instead of system().  system() is
 * used here per the project specification.
 * ═════════════════════════════════════════════════════════════════════ */
static int compile_source(const char *src_path, const char *out_path)
{
    /*
     * Build a gcc command that:
     *  -o out_path   : write binary here
     *  -Wall         : enable common warnings
     *  -O2           : optimise
     *  2>&1          : merge stderr into stdout so caller sees everything
     */
    char cmd[2048];
    int  ret = snprintf(cmd, sizeof(cmd),
                        "gcc -Wall -O2 -o '%s' '%s' 2>&1",
                        out_path, src_path);
    if (ret < 0 || (size_t)ret >= sizeof(cmd)) {
        fprintf(stderr, "[A] compile_source: path too long\n");
        return -1;
    }

    printf("[A] Running: %s\n", cmd);
    int status = system(cmd);

    if (status == -1) {
        perror("[A] system()");
        return -1;
    }
    /* WEXITSTATUS(status)==0 means gcc succeeded */
    if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[A] gcc exited with status %d\n",
                WEXITSTATUS(status));
        return -1;
    }

    /* Make the binary executable (mkstemp gives 0600) */
    if (chmod(out_path, 0755) != 0) {
        perror("[A] chmod");
        return -1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * connect_to_worker()
 *
 * Creates a TCP socket and connects to @ip:@port.
 * Returns the connected socket fd, or -1 on failure.
 * ═════════════════════════════════════════════════════════════════════ */
static int connect_to_worker(const char *ip, int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("[A] socket");
        return -1;
    }

    /* Set a connection timeout using SO_SNDTIMEO */
    struct timeval tv = { .tv_sec = NETWORK_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[A] Invalid IP address: %s\n", ip);
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[A] connect(%s:%d): %s\n", ip, port, strerror(errno));
        close(sockfd);
        return -1;
    }
    return sockfd;
}

/* ═══════════════════════════════════════════════════════════════════════
 * query_worker_load()
 *
 * Opens a short-lived connection to the worker's LOAD_QUERY_PORT,
 * sends MSG_QUERY_LOAD, and reads back a MSG_LOAD_RESPONSE.
 * Fills @info->load and sets @info->reachable accordingly.
 * ═════════════════════════════════════════════════════════════════════ */
static int query_worker_load(const char *ip, worker_info_t *info)
{
    info->reachable = 0;

    int sockfd = connect_to_worker(ip, LOAD_QUERY_PORT);
    if (sockfd < 0) return -1;

    set_socket_timeout(sockfd, 5);   /* short timeout for load probes */

    /* Send query header (no payload) */
    if (send_header(sockfd, MSG_QUERY_LOAD, FLAG_NONE, 0) != 0) {
        close(sockfd);
        return -1;
    }

    /* Receive response header */
    pkt_header_t h;
    if (recv_header(sockfd, &h) != 0) {
        close(sockfd);
        return -1;
    }

    if (h.type != MSG_LOAD_RESPONSE ||
        h.payload_len != sizeof(load_payload_t)) {
        fprintf(stderr, "[A] query_worker_load(%s): unexpected response "
                "(type=0x%02x payload_len=%u)\n",
                ip, h.type, h.payload_len);
        close(sockfd);
        return -1;
    }

    if (read_all(sockfd, &info->load, sizeof(load_payload_t)) != 0) {
        close(sockfd);
        return -1;
    }

    info->load.running_procs = ntohs(info->load.running_procs);
    info->load.total_procs   = ntohs(info->load.total_procs);

    info->reachable = 1;
    close(sockfd);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * select_best_worker()
 *
 * Lowest-Load policy: iterate @workers[0..count-1] and return the index
 * of the reachable worker with the smallest 1-minute load average.
 * Returns -1 if no reachable worker exists.
 * ═════════════════════════════════════════════════════════════════════ */
static int select_best_worker(worker_info_t *workers, int count)
{
    int   best_idx  = -1;
    float best_load = 1e30f;

    for (int i = 0; i < count; i++) {
        if (!workers[i].reachable) continue;
        if (workers[i].load.load_1min < best_load) {
            best_load = workers[i].load.load_1min;
            best_idx  = i;
        }
    }
    return best_idx;
}

/* ═══════════════════════════════════════════════════════════════════════
 * send_binary()
 *
 * Protocol for sending the binary:
 *
 *   ┌──────────────────────────────────────────────────────────────────┐
 *   │  MSG_SEND_BINARY header  (payload_len = file size in bytes)      │
 *   ├──────────────────────────────────────────────────────────────────┤
 *   │  raw binary data in CHUNK_SIZE blocks                            │
 *   └──────────────────────────────────────────────────────────────────┘
 *
 * The worker knows exactly how many bytes to read from payload_len.
 * ═════════════════════════════════════════════════════════════════════ */
static int send_binary(int sockfd, const char *bin_path)
{
    /* Get file size */
    struct stat st;
    if (stat(bin_path, &st) != 0) {
        perror("[A] stat binary");
        return -1;
    }
    uint32_t file_size = (uint32_t)st.st_size;

    /* Open binary */
    int fd = open(bin_path, O_RDONLY);
    if (fd < 0) {
        perror("[A] open binary");
        return -1;
    }

    /* Send header announcing the payload size */
    if (send_header(sockfd, MSG_SEND_BINARY, FLAG_NONE, file_size) != 0) {
        perror("[A] send_header");
        close(fd);
        return -1;
    }

    /* Stream binary in chunks */
    char   chunk[CHUNK_SIZE];
    size_t total_sent = 0;

    while (total_sent < file_size) {
        size_t to_read = CHUNK_SIZE;
        size_t remaining = file_size - total_sent;
        if (to_read > remaining) to_read = remaining;

        ssize_t n = read(fd, chunk, to_read);
        if (n <= 0) {
            fprintf(stderr, "[A] read binary chunk: %s\n",
                    n < 0 ? strerror(errno) : "unexpected EOF");
            close(fd);
            return -1;
        }

        if (write_all(sockfd, chunk, (size_t)n) != 0) {
            perror("[A] write_all chunk");
            close(fd);
            return -1;
        }
        total_sent += (size_t)n;
    }

    printf("[A] Sent %zu byte(s).\n", total_sent);
    close(fd);

    /* Wait for ACK from worker before blocking on output */
    pkt_header_t ack;
    if (recv_header(sockfd, &ack) != 0 || ack.type != MSG_ACK) {
        fprintf(stderr, "[A] Did not receive ACK after binary send.\n");
        return -1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * receive_output()
 *
 * Reads MSG_EXEC_RESULT from the worker and prints to stdout.
 * Also handles MSG_ERROR gracefully.
 * ═════════════════════════════════════════════════════════════════════ */
static int receive_output(int sockfd)
{
    pkt_header_t h;
    if (recv_header(sockfd, &h) != 0) {
        fprintf(stderr, "[A] receive_output: connection closed unexpectedly\n");
        return -1;
    }

    if (h.type == MSG_ERROR) {
        /* Read the error payload */
        uint32_t elen = h.payload_len;
        if (elen > 512) elen = 512;
        char errbuf[513] = {0};
        read_all(sockfd, errbuf, elen);
        fprintf(stderr, "[A] Worker returned error: %s\n", errbuf + 1);
        return -1;
    }

    if (h.type != MSG_EXEC_RESULT) {
        fprintf(stderr, "[A] receive_output: unexpected message type 0x%02x\n",
                h.type);
        return -1;
    }

    /* Stream output to stdout */
    printf("\n"
           "╔══════════════════════════════════════════════════════════╗\n"
           "║              Remote Execution Output                    ║\n"
           "╚══════════════════════════════════════════════════════════╝\n");

    uint32_t remaining = h.payload_len;
    char     chunk[CHUNK_SIZE];

    while (remaining > 0) {
        uint32_t to_read = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
        ssize_t  n = read(sockfd, chunk, to_read);
        if (n <= 0) break;
        fwrite(chunk, 1, (size_t)n, stdout);
        remaining -= (uint32_t)n;
    }

    printf("\n"
           "╚══════════════════════════════════════════════════════════╝\n");
    fflush(stdout);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * set_socket_timeout()
 * ═════════════════════════════════════════════════════════════════════ */
static void set_socket_timeout(int sockfd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* ═══════════════════════════════════════════════════════════════════════
 * print_usage()
 * ═════════════════════════════════════════════════════════════════════ */
static void print_usage(const char *progname)
{
    fprintf(stderr,
            "Usage: %s <source.c> <worker_ip1> [worker_ip2 ...]\n"
            "\n"
            "  source.c    — C source file to compile and distribute\n"
            "  worker_ipN  — IPv4 address of a Machine B worker node\n"
            "\n"
            "Example:\n"
            "  %s task.c 192.168.1.10 192.168.1.11\n",
            progname, progname);
}
