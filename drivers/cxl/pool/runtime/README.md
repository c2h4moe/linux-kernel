# CXL Macro Runtime Demo

This directory contains a user-space prototype for "macro-process" execution:

- one process per simulated machine,
- shared CXL virtual-address window across machines,
- remote spawn by command queue in CXL shared memory,
- strong mapping barrier for shared allocations.

## Build

```bash
cd drivers/cxl/pool/runtime
make
```

## Run

Load the kernel module first (example):

```bash
insmod /path/to/cxl_pool.ko machine_id=0
```

Run demo with 2 simulated machines:

```bash
./cxl_macro_demo 2
```

Expected success line:

```text
PASS: cxl_macro_demo succeeded
```
