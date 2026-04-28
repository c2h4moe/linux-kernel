# CXL Macro Runtime Demo

This directory contains a user-space prototype for "macro-process" execution:

- one process per simulated machine,
- shared CXL virtual-address window across machines,
- remote worker spawn by command queue in CXL shared memory,
- strong mapping barrier for shared allocations,
- one fixed macro-process,
- no heartbeat, membership, or failure recovery logic.

The user-facing layer now provides a simpler API:

- `cxl_run()` to launch the macro-process,
- `CXL_WORKER(name)` to register a remote-spawnable worker,
- `cxl_spawn(worker, machine, shared_ptr)` to start that worker on another machine,
- `cxl_malloc_current()` / `cxl_free_current()` for shared allocations from the current thread,
- `cxl_mutex_init/destroy/create/free/lock/unlock/trylock` for shared spin-based mutexes.

`cxl_mutex` is a shared-memory mutex implemented with atomics and spin backoff.
The lock object itself must live in the CXL shared window, either:

- embedded in a shared struct and initialized with `cxl_mutex_init()`, or
- allocated dynamically with `cxl_mutex_create()`.

Runtime shutdown policy in the current simulator:

- `machine 0` is treated as the main machine for one macro-process run.
- When the `machine 0` child process exits, the parent supervisor sends `SIGTERM`
  to the remaining machine processes and reaps them.
- The demo therefore does not need mailbox-based exit signaling.

## Build

```bash
cd drivers/cxl/pool/runtime
make
```

## Run

Load the kernel module first (example):

```bash
insmod /path/to/cxl_pool.ko
```

Run demo with 2 simulated machines:

```bash
./cxl_macro_demo 2
```

Run the mutex demo with 2 simulated machines:

```bash
./cxl_mutex_demo 2
```

Expected success line:

```text
PASS: cxl_macro_demo succeeded
PASS: cxl_mutex_demo succeeded
```
