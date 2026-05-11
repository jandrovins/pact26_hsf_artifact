# Multisaxpy -- Multiple SAXPY Operations -- Baseline (nosvorig)

## What it computes

Repeated SAXPY (Y = alpha * X + Y) operations on vector blocks of size TS. Each iteration performs the operation on all blocks. This is a memory-bandwidth-bound workload used to evaluate scheduling overhead and data locality effects.

## Task scheduling

- One task per block: `axpy_task(block_id)`
- Optional priority: `priority(num_blocks - block_id)` when `VVV_PRIO_ENABLED=1` — lower block IDs get higher priority, encouraging spatial locality
- `immediate_successor` controlled per config (true/false)

## Memory allocation

- `VVV_MMAP_ENABLED=1`: uses mmap for large allocations (huge pages)
- `VVV_MMAP_ENABLED=0`: uses standard malloc
- NUMA initialization via first-touch from the task that first writes each block

Note: The "orig" (no NUMA init, no mmap) variant was too slow to produce results and is not reproduced. Only the "init" variant with NUMA-aware settings is included.

## Task affinity

No explicit task-to-topology mapping. Tasks execute on any available CPU. The only locality mechanism is the priority ordering (when enabled), which encourages consecutive blocks to execute in order on the same CPU.

## How to build and run

```bash
cd nosvorig/multisaxpy
nix develop --impure ../
make
./b6_multisaxpy_prio 56524800 29440 1000
#                     N        TS    iterations
```

## Reproduce results

```bash
./launcher_reproduce.sh
cd ../..
python collect_results.py
```
