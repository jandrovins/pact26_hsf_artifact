# Matrix Multiplication -- Baseline (nosvorig)

## What it computes

Tiled dense matrix multiplication C = A * B using BLAS DGEMM. The matrices are divided into tiles of size TS x TS, and each tile multiplication is a separate OmpSs-2 task. Uses MPI (single rank, TAMPI interop) for the communication pattern.

## Task scheduling

Each DGEMM tile operation is submitted as an OmpSs-2 task with data dependencies on the input tiles (A, B) and output tile (C). No explicit priority ordering. `immediate_successor=true` enables fast handoff between dependent tasks on the same CPU.

## Memory allocation

- **orig variant**: Standard mmap allocation, `numa=0`
- **init variant**: NUMA-aware allocation, `numa=1` — pages are interleaved across NUMA nodes for balanced bandwidth

## Task affinity

No explicit task-to-topology mapping. DGEMM tasks execute on any available CPU. Data locality depends on first-touch page placement.

## How to build and run

```bash
cd nosvorig/matmul
nix develop --impure ../
make
./build/02.matmul_ompss2_itampi.bin 6144 6144 20 256 1
#                                    N    M    ITS TS  WARMUP
```

## Reproduce results

```bash
./launcher_reproduce.sh
cd ../..
python collect_results.py
```
