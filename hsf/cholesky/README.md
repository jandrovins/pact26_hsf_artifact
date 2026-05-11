# Cholesky Decomposition -- HSF (Taskgroup-Optimized)

## What it computes

Block-tiled Cholesky factorization of a symmetric positive-definite matrix (same algorithm as baseline). Kernels: POTRF, TRSM, SYRK, GEMM.

## Taskgroup optimizations

### Dual-pool strategy

The scheduler uses **two separate taskgroup pools** with different policies:

1. **PRIO pool (POTRF/TRSM/SYRK)**: Priority-based taskgroups, one per topology domain. Tasks are assigned to taskgroups by column index in round-robin: `tg_id = column_idx % num_domains`. Priorities encode the critical path:
   - POTRF: `20000 + nblocks - k`
   - TRSM: `30000 + nblocks - i`
   - SYRK: `10000 + nblocks - i`

2. **FIFO pool (GEMM)**: Separate FIFO taskgroup pool for GEMM tasks. Consecutive GEMM tasks (in creation order) are batched into the same taskgroup (`gemm_tiles_per_block=48`). The counter resets each Cholesky iteration k, providing temporal locality within each step.

### Affinity configuration

Taskgroups are pinned to hardware topology domains via `tglib`:

- **Small problem (N=6144)**: Flexible affinity — preferred at CCD level (`lower=cs`), acceptable at node level (`upper=node`). This prioritizes load balance when the working set fits in L3 cache.
- **Large problem (N=33792)**: Strict NUMA affinity (`lower=numa`, `upper=numa`, `flex=0`). Reduces remote memory traffic for the larger working set.

### Task-to-data mapping

- POTRF/TRSM/SYRK tasks operating on tiles in the same matrix column share a taskgroup
- Columns are distributed round-robin across topology domains: `tg_id = col % N_domains`
- GEMM tasks are batched: every 48 consecutive GEMMs share a taskgroup, assigned linearly across the FIFO pool

## Memory allocation

Uses `tglib_mmap_wrapper()` for matrix allocation with `MAP_HUGETLB` support. Init tasks are assigned to taskgroups matching their column, providing first-touch NUMA placement aligned with the computation mapping.

## How to build and run

```bash
cd hsf/cholesky
nix develop --impure ../  # Enter nix devshell (haffsched nOS-V + tglib)
make                      # Builds cholesky_oss.bin
export VVV_TG_ENABLED=1 VVV_LOWER_LVL=cs VVV_UPPER_LVL=node VVV_AFF_FLEXIBLE=1
export VVV_PRIORITY_ENABLED=1 VVV_CHOL_GEMM_TILES_PER_BLOCK=48
./cholesky_oss.bin 6144 256 1
```

## Reproduce results

```bash
./launcher_reproduce.sh   # Submits SLURM jobs (3 reps per config)
# After jobs complete:
cd ../..
python collect_results.py
```
