# Heat Equation Solver -- Baseline (nosvorig)

## What it computes

2D heat diffusion solver using Gauss-Seidel iteration. The domain is discretized into a grid of blocks (block size `bs`), and each iteration updates all interior blocks via a 5-point stencil computation. Each block depends on its 4 neighbors (N/S/E/W), creating a wavefront dependency pattern across iterations.

## Task scheduling

- Tasks use OmpSs-2 dependencies: `in(reps[R-1][C], reps[R+1][C], reps[R][C-1], reps[R][C+1]) inout(reps[R][C])`
- Priority scheduling: `priority(maxIt - it)` — later iterations get higher priority, encouraging the wavefront to advance
- `immediate_successor=true`: when a task completes, its successor can immediately execute on the same CPU

## Memory allocation

- **orig variant**: Standard allocation with `VVV_MMAP_ENABLED=1`, no NUMA interleaving
- **init variant**: `VVV_NUMA_INTERLEAVED=1` — pages are distributed across NUMA nodes via interleaved mmap policy

## Task affinity

No explicit task-to-topology binding. All tasks can run on any available CPU. Locality arises only from first-touch placement of the block data and the scheduler's tendency to keep tasks on the same CPU via immediate successor.

## How to build and run

```bash
cd nosvorig/heat
nix develop --impure ../
make
./02.heat_ompss2_prio.bin -r 12288 -c 12288 -t 800 -b 256
```

## Reproduce results

```bash
./launcher_reproduce.sh
cd ../..
python collect_results.py
```
