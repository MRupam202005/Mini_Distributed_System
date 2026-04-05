# Distributed Task Execution System (DTE)

A minimal, production-aware distributed execution framework written in **pure C**. It uses Linux TCP sockets, `fork()`/`exec()`, and `/proc/loadavg` for dynamic load-balanced scheduling.

---

## 1. Quick Start (Local Execution)

Follow these steps to run the system on your own machine using `127.0.0.1`.

### Step 1: Build the Project
Compile the Server and Worker binaries.
```bash
make all
```

### Step 2: Start the Worker
Open a terminal and run the worker. It will stay active, waiting for tasks.
```bash
./worker
```

### Step 3: Dispatch a Task (Server)
Open a **second terminal** and send the sample task to the local worker.
```bash
./server task_example.c 127.0.0.1
```

---

## 2. How It Works (Workflow)

The system follows a specific sequence of events to ensure tasks are executed on the best available machine.

### The Protocol "Handshake"

| Step | Server (Coordinator) | Worker |
| :--- | :--- | :--- |
| **1** | Compiles `.c` source → Binary | *Listening on ports 9100/9101* |
| **2** | **Query Load:** "How busy are you?" → | ← Reads `/proc/loadavg` and responds |
| **3** | Picks best worker & connects | *Forking child to handle execution* |
| **4** | **Send Task:** Streams binary data → | ← ACKs receipt & saves to `/tmp` |
| **5** | *Waiting for results...* | **Executes:** Runs binary & captures output |
| **6** | **Receive Result:** Displays output | ← Sends captured text & deletes binary |

### Entity Roles
- **Server (Coordinator):** The brain. It compiles code, checks worker health/load, and dispatches binaries.
- **Worker:** The muscle. It reports its CPU load, receives binaries, and executes them in an isolated process.

---

## 3. Project Structure

```text
.
├── server.c          # Server source (Dispatcher)
├── worker.c          # Worker daemon source (Executor)
├── protocol.h        # Shared wire-format definitions & I/O helpers
├── task_example.c    # Sample task (Fibonacci calculation)
├── Makefile          # Build system (make all, make clean)
└── README.md         # This documentation
```

---

## 4. Technical Reference

### Network Ports
- **9100 (Exec Port):** Used for binary transfer and receiving execution results.
- **9101 (Load Port):** Used for quick load-balancing queries.

### Architecture Highlights
- **Concurrency:** Worker handles multiple simultaneous clients using `fork()`.
- **Load Balancing:** Server always selects the worker with the lowest **1-minute load average**.
- **Execution:** Tasks are run via `execv()` with stdout/stderr redirected into a pipe for capture.
- **Cleanup:** Temporary binaries are unlinked immediately after execution to save disk space.

---

## 5. Security Considerations

> [!WARNING]  
> This system is designed for **trusted LAN environments** or local testing. It has **no authentication or encryption**.

| Risk | Mitigation |
| :--- | :--- |
| **Arbitrary Execution** | Run `worker` as a low-privilege user; Use firewalls to restrict access. |
| **Plaintext Transport** | Avoid running on public Wi-Fi; Use a VPN or SSH tunnel for remote work. |
| **No Authentication** | Only expose ports 9100/9101 to trusted internal IP addresses. |

---

## 6. Advanced Usage (LAN Setup)

To run this system across multiple physical machines, follow these steps:

### Step 1: Identify IP Addresses
On each **Worker (Device 2, 3...)**, find the local IP address:
```bash
ip addr show | grep "inet "
# Look for 192.168.x.x
```

### Step 2: Configure Firewalls
On each Worker, you must allow incoming TCP traffic on ports **9100** and **9101**.
```bash
# Ubuntu/Debian (UFW)
sudo ufw allow 9100/tcp
sudo ufw allow 9101/tcp
```

### Step 3: Start Workers
Run `./worker` on every worker machine.

### Step 4: Dispatch from Server
On the **Server (Device 1)**, launch `server` with the worker IPs:
```bash
# Single remote worker
./server task.c 192.168.1.10

# Multi-worker load balancing
./server task.c 192.168.1.10 192.168.1.11 192.168.1.12
```

### Troubleshooting
- **Ping Check:** Ensure `ping [Worker_IP]` works from the Server.
- **Connection Timeout:** Usually caused by a **firewall** blocking ports 9100/9101 on the worker side.
- **Connection Refused:** Ensure `worker` is actually running on the worker.

---

## 7. Makefile Commands
- `make all`: Build both binaries.
- `make clean`: Remove binaries and build artifacts.
- `make install`: Install as `dte_coordinator` and `dte_worker` to `/usr/local/bin` (requires sudo).
