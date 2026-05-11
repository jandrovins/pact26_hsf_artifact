# Cholesky Decomposition -- Baseline (nosvorig)

## What it computes

Block-tiled Cholesky factorization of a symmetric positive-definite matrix. The algorithm decomposes a matrix A into L * L^T using four BLAS/LAPACK kernels operating on tiles of size TS x TS:

- **POTRF**: Cholesky factorization of a diagonal tile (highest priority)
- **TRSM**: Triangular solve for tiles in the same column (medium priority)
- **SYRK**: Symmetric rank-k update for diagonal tiles (lower priority)
- **GEMM**: General matrix multiply for off-diagonal tiles

## Task scheduling

Tasks are submitted with **priority-based scheduling** reflecting the critical path:
- POTRF tasks have the highest priority (they unblock the most downstream work)
- TRSM and SYRK follow
- GEMM tasks are the bulk of computation but are not on the critical path

The baseline uses the standard nOS-V scheduler (released v4.0.0) with `immediate_successor=true`, which allows a finishing task to immediately hand off to its successor on the same CPU.

## Memory allocation

- Uses `valloc()` (NUMA-aware allocator) for the matrix
- **orig variant**: Standard allocation without explicit NUMA interleaving
- **init variant**: NUMA-interleaved allocation (`VVV_NUMA_INTERLEAVED=1`) distributes pages across NUMA nodes

Matrix tiles are initialized via OmpSs-2 tasks (`#pragma oss task label("init_tile")`), providing first-touch NUMA placement: each tile is physically allocated on the NUMA node of the core that first writes to it.

## Task affinity

No explicit task-to-topology mapping. Tasks run on any available CPU. The nOS-V `task_affinity.default = "all"` setting means tasks have no affinity constraints.

The only locality mechanism is first-touch from the initialization tasks.

## How to build and run

```bash
cd nosvorig/cholesky
nix develop --impure ../  # Enter nix devshell
make                      # Builds cholesky_oss.bin
./cholesky_oss.bin 6144 256 1   # N=6144, TS=256, 1 iteration
```

## Reproduce results

```bash
./launcher_reproduce.sh   # Submits SLURM jobs (3 reps per config)
# After jobs complete:
cd ../..
python collect_results.py # Generates summary.csv
```
