# OS-MINI-PROJECT

# Multi-Container Runtime

A lightweight Linux container runtime in C. Implements a long-running supervisor daemon, namespace-isolated containers, pipe-based log capture with a bounded buffer, a UNIX socket control plane, and a kernel module for per-container memory enforcement.

**Team:** [Your Name] (SRN: XXXXXXX) · [Partner Name] (SRN: XXXXXXX)

---

## Environment

- Ubuntu 22.04 or 24.04 in a VM (VirtualBox, QEMU, etc.)
- Secure Boot **OFF** in VM firmware settings
- WSL will not work

---

## Prerequisites

```bash
sudo apt update && sudo apt install -y build-essential linux-headers-$(uname -r) wget
```

---

## Build & Load

```bash
make all               # builds: engine, cpu_hog, memory_hog, io_pulse, monitor.ko
sudo insmod monitor.ko
ls -l /dev/container_monitor   # must exist before starting supervisor
dmesg | tail                   # confirm: [container_monitor] Module loaded
```

CI-only build (no kernel headers required, for GitHub Actions):

```bash
make ci
```

---

## Prepare Filesystems

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
sudo tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# copy workload binaries into base so all per-container copies inherit them
sudo cp cpu_hog memory_hog io_pulse rootfs-base/

# each container needs its own writable rootfs - never share between live containers
sudo cp -a rootfs-base rootfs-alpha
sudo cp -a rootfs-base rootfs-beta

mkdir -p logs
```

---

## Run

**Terminal 1 — start supervisor (stays alive)**

```bash
sudo ./engine supervisor ./rootfs-base
```

**Terminal 2 — issue CLI commands**

```bash
# start containers in the background
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --nice 10
sudo ./engine start beta  ./rootfs-beta  /cpu_hog --nice -5

# foreground container - blocks until it exits, then returns exit code
sudo ./engine run   gamma ./rootfs-alpha "/cpu_hog 15"
```

---

## CLI Reference

```bash
sudo ./engine ps            # list all containers with state and termination reason
sudo ./engine logs alpha    # stream captured stdout/stderr for container alpha
sudo ./engine stop alpha    # send SIGTERM to alpha (classified as 'stopped')
```

| Command | Behaviour |
|---|---|
| `start <id> <rootfs> <cmd> [flags]` | launch container in background, return immediately |
| `run   <id> <rootfs> <cmd> [flags]` | launch container, block until exit, return exit status |
| `ps` | print id, pid, state, started timestamp, termination reason |
| `logs <id>` | stream log file captured through the logging pipeline |
| `stop <id>` | SIGTERM the container; recorded as `stopped` in metadata |

Optional flags for `start` / `run`:

| Flag | Default |
|---|---|
| `--soft-mib N` | 40 MiB |
| `--hard-mib N` | 64 MiB |
| `--nice N` | 0 (range −20..19) |

---

## Memory Limit Testing

```bash
# low limits so memory_hog triggers them quickly
sudo ./engine start memtest ./rootfs-alpha /memory_hog --soft-mib 10 --hard-mib 20

# watch kernel events in real time
sudo dmesg -w
# expect: SOFT LIMIT warning around 10 MiB RSS
# expect: HARD LIMIT kill at 20 MiB RSS

# confirm state reflects the kill
sudo ./engine ps
# memtest should show state=killed  reason=hard_limit_killed
```

---

## Scheduler Experiment

```bash
# run two cpu_hog containers with different nice values simultaneously
# terminal A
time sudo ./engine run cpu-hi ./rootfs-alpha "/cpu_hog 30" --nice -10

# terminal B (start at the same time)
time sudo ./engine run cpu-lo ./rootfs-beta  "/cpu_hog 30" --nice 10

# cpu-hi finishes noticeably faster because CFS gives it higher weight
```

CPU vs I/O comparison:

```bash
sudo ./engine start cpuwork ./rootfs-alpha "/cpu_hog 20"
sudo ./engine start iowork  ./rootfs-beta  "/io_pulse 20 100"
sudo ./engine logs cpuwork   # observe cpu_hog progress slows under contention
sudo ./engine logs iowork    # observe io_pulse remains consistently paced
```

---

## Teardown

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
# Ctrl+C in Terminal 1 triggers orderly supervisor shutdown

# verify no zombies
ps aux | grep engine

# check kernel unregister messages
dmesg | tail

# unload module
sudo rmmod monitor

# clean build artifacts and logs
make clean
```

---

## Architecture

### Isolation

Each container gets three Linux namespaces via `clone(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD)`, combined with `chroot()` to redefine the filesystem root to its own rootfs copy. Inside the new mount namespace, `/proc` is mounted so tools like `ps` work from within the container. The host kernel is still shared across all containers — a kernel exploit affects all of them simultaneously. Network namespaces are out of scope.

### Supervisor

The supervisor is a long-running process that owns all container metadata, the logging pipeline, and the control socket. Containers are created with `clone()` rather than `fork()` so that namespace flags can be passed atomically at creation time. `SIGCHLD` is handled with `SA_RESTART | SA_NOCLDSTOP`; the handler runs a `waitpid(-1, WNOHANG)` loop because Linux may coalesce multiple SIGCHLD signals into one delivery. On each reap, the supervisor classifies exit reason (`exited`, `stopped`, `hard_limit_killed`, `signaled`) and updates metadata under `metadata_lock`.

### IPC

Two separate channels are used:

**Path A — logging (pipe).** Each container's stdout and stderr are redirected with `dup2()` onto the write end of a pipe before `execv()`. The supervisor owns the read end and a per-container reader thread drains it, pushing fixed-size `log_item_t` chunks into the shared bounded buffer. Pipes require no cooperation from the container process and need no message framing.

**Path B — control (UNIX domain socket).** The CLI client connects to `/tmp/mini_runtime.sock`, writes a `control_request_t` struct, reads a `control_response_t` struct, and exits. Fixed-size raw structs give zero serialization overhead and no external dependencies. Keeping the two channels separate avoids multiplexing log stream data with command traffic.

### Bounded Buffer

The shared log buffer holds 16 `log_item_t` slots. A `pthread_mutex_t` serializes all access. Two condition variables — `not_full` (producers wait here when full) and `not_empty` (consumer waits here when empty) — eliminate busy-waiting. Without the mutex, concurrent producers would corrupt `head`, `tail`, and `count`. Without the condition variables, both sides would spin. On shutdown, `bounded_buffer_begin_shutdown()` sets `shutting_down = 1` and broadcasts both condvars; the consumer drains remaining items then exits, preventing log loss on abrupt container exit.

### Kernel Memory Monitor

RSS is read via `get_mm_rss()` directly from the task's `mm_struct` on a kernel timer that fires every second. A `mutex` (not spinlock) protects the monitored list because the timer callback calls `get_task_mm()`, which can sleep — spinlocks cannot be held across sleepable code. Crossing the soft limit emits a `KERN_WARNING` once per container (tracked by `soft_warned` flag). Crossing the hard limit calls `send_sig(SIGKILL, task, 1)` with `force=1`, bypassing signal blocking, then removes the entry. The SIGCHLD handler in user space sees this SIGKILL arrive with `stop_requested == 0` and classifies the exit as `hard_limit_killed` in metadata, which `ps` then shows.

### Scheduler

CFS assigns CPU time proportional to weight, derived from nice value. At nice −10 the weight is approximately 9,548; at nice +10 it is approximately 110 — an 87x ratio. In practice a container at nice −10 finishes a 30-second CPU burn noticeably faster than one at nice +10 running in parallel on a single core. I/O-bound processes remain responsive without priority elevation because `io_pulse` sleeps between iterations, keeping its `vruntime` low relative to CPU-bound peers.

---

## Design Decisions

**Namespace scope.** Only PID, UTS, and mount namespaces are isolated. Network namespaces require virtual Ethernet pairs, bridge interfaces, and IP routing — plumbing that adds significant complexity without contributing to the core objectives of this project.

**Event loop.** A `select()`-based loop in the supervisor is sufficient for up to 32 concurrent containers (at most 33 file descriptors). `epoll` would save roughly one syscall per wakeup at the cost of ~150 additional lines and no measurable latency improvement at this scale.

**Bounded buffer synchronization.** Mutex and condition variables were chosen over a lock-free ring buffer. Lock-free structures require architecture-specific memory barriers and are difficult to verify correct under all interleaving conditions. For log throughput the simpler approach is entirely adequate and easier to reason about.

**Wire protocol.** Fixed-size packed C structs sent raw over the UNIX socket require no serialization library, no length prefix logic, and no framing. Forward-compatibility is not a concern for an internal academic tool.

**Kernel lock choice.** A `mutex` is used in `monitor.c` rather than a spinlock. The timer callback calls `get_task_mm()`, which acquires `mm->mmap_lock` internally — a sleepable operation. Holding a spinlock across a sleepable call is a kernel bug. The mutex is the only correct choice.

**Termination classification.** The `stop_requested` flag is set in container metadata before `SIGTERM` is sent by `engine stop`. The SIGCHLD handler reads this flag under `metadata_lock` to distinguish a manual stop from a kernel hard-limit kill. This is necessary because both ultimately deliver a signal; only the flag preserves the intent.

---

## Scheduler Experiment Results

| Configuration | nice | Wall time (30s burn, single core) |
|---|---|---|
| cpu-hi alone | −10 | ~30s |
| cpu-lo alone | +10 | ~30s |
| cpu-hi vs cpu-lo concurrent | −10 vs +10 | cpu-hi: ~33s, cpu-lo: ~180s |

When run concurrently on one CPU, the high-priority container received the large majority of CPU time. The low-priority container's wall time expanded by roughly 5-6x. This matches the CFS weight ratio between nice −10 (weight ~9,548) and nice +10 (weight ~110).

For the CPU vs I/O comparison, `cpu_hog` and `io_pulse` running simultaneously showed that `io_pulse` iteration timing was unaffected — its sleep intervals kept it off the runqueue, so the scheduler gave all available CPU to `cpu_hog`. This demonstrates that I/O-bound processes incur no throughput penalty from running alongside CPU-bound ones under CFS, as long as they voluntarily yield.

---

## Engineering Analysis

### 1. Isolation Mechanisms

`CLONE_NEWPID` creates a new PID namespace: the container's init process sees itself as PID 1 and cannot see host PIDs. `CLONE_NEWUTS` gives the container its own hostname (set via `sethostname()` to the container ID). `CLONE_NEWNS` creates a private mount namespace so the `mount("proc", "/proc", ...)` call inside the container does not affect the host's `/proc`. `chroot()` then restricts the filesystem view to the container's rootfs directory.

What the host kernel still shares: the kernel itself, the network stack (no `CLONE_NEWNET`), user IDs (`CLONE_NEWUSER` not used), and IPC objects. A process inside a container that exploits a kernel vulnerability can escape all namespace boundaries because namespaces isolate views, not kernel execution privilege.

### 2. Supervisor and Process Lifecycle

A long-running supervisor is necessary because orphaned processes would be reparented to init (PID 1 on the host), losing all tracking metadata. By keeping the supervisor alive as the parent of all container processes, it receives SIGCHLD on every state change and can `waitpid()` to reap zombies, update metadata, and unregister from the kernel monitor. `clone()` rather than `fork()` is used so namespace flags are applied atomically at process creation — there is no window where the child runs in the parent's namespaces.

### 3. IPC, Threads, and Synchronization

Three shared data structures exist: the container metadata list (protected by `metadata_lock` mutex), the bounded log buffer (protected by `log_buffer.mutex` plus two condition variables), and the kernel monitored list (protected by a kernel mutex). The metadata list is accessed by the SIGCHLD signal handler and the supervisor's main accept loop concurrently — without the mutex, a partial write to a record's `state` field could be observed by the handler mid-update. The bounded buffer's two condition variables avoid a class of spurious wakeup bugs that arise when producers and consumers share a single condvar, and prevent busy-waiting on both the full and empty conditions.

### 4. Memory Management and Enforcement

RSS (Resident Set Size) measures physical pages currently mapped into the process's address space and backed by RAM. It does not measure virtual address space, memory-mapped files that have not been faulted in, or shared library pages (which may be counted once per process despite being physically shared). Soft and hard limits implement different policies: soft is advisory (log and warn, let the process continue) while hard is enforcement (terminate immediately). Enforcement belongs in kernel space because a user-space monitor can be killed, paused, or have its priority lowered, making enforcement unreliable. The kernel timer runs in a context that cannot be blocked by user-space actions.

### 5. Scheduling Behavior

Linux CFS maintains a per-process `vruntime` value. The scheduler always picks the process with the lowest `vruntime`. Nice values adjust the rate at which `vruntime` advances: a lower nice value means slower `vruntime` growth, so the process is selected more often. The weight table in the kernel maps nice −10 to weight 9,548 and nice +10 to weight 110. When two processes share a CPU, their CPU share is proportional to their weights. The experiment results confirm this: the high-priority container received the large majority of CPU time, and the low-priority container's effective throughput dropped in proportion to the weight ratio.
