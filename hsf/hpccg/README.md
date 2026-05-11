# HPCCG -- Sparse Conjugate Gradient Solver -- HSF (Taskgroup-Optimized)

## What it computes

Conjugate gradient solver for sparse systems (same algorithm as baseline). Kernels: SpMV, DDOT, WAXPBY.

## Taskgroup optimizations

### Dual taskgroup pools

Two independent taskgroup pools managed by `TaskGroupManager`:

1. **Main pool (WAXPBY, DDOT, init, MPI exchange)**: Linear row-based partition across topology domains. Task assignment: `tglib_get_taskgroup_linear(row, nrows)`. Uses PRIO policy by default.

2. **SpMV-specific pool**: Separate pool with FIFO policy, preserving row-traversal order to maximize sequential L3 cache reuse. Enabled via `VVV_SPMV_TG_ENABLED=1`.

### Affinity configuration

- Strict CCD-level affinity (`lower=cs`, `upper=cs`, `flex=0`) for both pools
- SpMV pool uses FIFO ordering within each CCD, so consecutive matrix rows processed by the same CCD are traversed in order

### Immediate-successor mode for SpMV

`VVV_SPMV_IMM=1`: When a SpMV task completes, the next SpMV task in the same taskgroup starts on the same CPU. This maximizes cache reuse for sequential row access patterns in the sparse matrix.

### Task-to-data mapping

- **Vector operations (WAXPBY, DDOT)**: Rows partitioned into contiguous blocks, assigned linearly across CCD domains. Task for row range `[start, start+bs)` maps to `tg_id = start * N_domains / N_rows`.
- **SpMV**: Same row-based mapping but with FIFO policy. Block distribution uses `VVV_BS_FACTOR=4` to control how many consecutive blocks map to the same CCD.

## Memory allocation

Uses mmap with `VVV_MMAP_ENABLED=1`. Matrix and vector data initialized via tasks assigned to matching taskgroups for first-touch NUMA placement.

## How to build and run

```bash
cd hsf/hpccg
nix develop --impure ../
make HPCCG_mpi_oss-notampi.bin
export VVV_TG_ENABLED=1 VVV_LOWER_LVL=cs VVV_UPPER_LVL=cs VVV_AFF_FLEXIBLE=0
export VVV_BS_FACTOR=4 VVV_SPMV_TG_ENABLED=1 VVV_SPMV_POLICY=FIFO VVV_SPMV_IMM=1
./HPCCG_mpi_oss-notampi.bin t1 288 192 768 100 1536
```

## Reproduce results

```bash
./launcher_reproduce.sh
cd ../..
python collect_results.py
```
