# HPCCG -- Sparse Conjugate Gradient Solver -- Baseline (nosvorig)

## What it computes

Conjugate gradient (CG) iterative solver for sparse systems Ax = b. Three main kernels:
- **SpMV**: Sparse matrix-vector product (memory bandwidth limited)
- **DDOT**: Dot product (reduction)
- **WAXPBY**: Weighted vector sum (y = alpha*x + beta*y)

The sparse matrix is generated from a 3D grid of dimensions nx x ny x nz with a 27-point stencil. Single MPI rank (no-tampi variant).

## Task scheduling

Tasks are created per kernel invocation with block-level parallelism. The number of tasks is configurable via the `ntasks` parameter. Tasks have no explicit priority or affinity.

`immediate_successor=true` is enabled, allowing a finishing task to immediately schedule its successor on the same CPU.

## Memory allocation

- **orig variant**: `VVV_MMAP_ENABLED=1`, standard allocation, `numainterleaved=0`
- **init variant**: `VVV_NUMA_INTERLEAVED=1` — memory pages are distributed across NUMA nodes

## Task affinity

No explicit task-to-topology mapping. Tasks are assigned to any available CPU. Data locality depends only on the OS page placement and cache effects from task execution order.

## How to build and run

```bash
cd nosvorig/hpccg
nix develop --impure ../
make HPCCG_mpi_oss-notampi.bin
./HPCCG_mpi_oss-notampi.bin t1 288 192 768 100 1536
```

## Reproduce results

```bash
./launcher_reproduce.sh
cd ../..
python collect_results.py
```
