# HSF: A Hierarchical Scheduling Framework — Artifact

This repository contains the benchmarks and reproduction scripts for the PACT'26 paper *HSF: A Hierarchical Scheduling Framework*.

## Repository structure

```
pact_consolidated/
├── hsf/                        # HSF-optimized benchmarks (taskgroup variant)
│   ├── flake.nix               # Nix flake (nOS-V haffsched + NODES haffsched-debug2)
│   ├── cholesky/
│   ├── heat/
│   ├── hpccg/
│   ├── matmul/
│   └── multisaxpy/
├── nosvorig/                   # OmpSs-2 baseline benchmarks (unmodified nOS-V v4.0.0)
│   ├── flake.nix               # Nix flake (nOS-V v4.0.0 + NODES 1.4.0)
│   ├── cholesky/
│   ├── heat/
│   ├── hpccg/
│   ├── matmul/
│   └── multisaxpy/
├── baseline/                   # External baseline (pure OpenMP/MPI, no OmpSs-2)
│   ├── flake.nix               # Nix flake (clang + OpenMP + MPI + AMD BLIS/libFLAME)
│   ├── cholesky/               # LAPACKE_dpotrf via libFLAME+BLIS
│   ├── heat/                   # OpenMP Gauss-Seidel
│   ├── hpccg/                  # MPI+OpenMP CG
│   ├── matmul/                 # OpenMP cblas_dgemm (mt-dgemm)
│   └── multisaxpy/             # OpenMP SAXPY
├── results/                    # Original paper results (10-30 reps)
├── reproduced_results/         # Reproduced results (3 reps per config)
├── collect_results.py          # Parse SLURM output -> summary CSVs
├── plot_reproduced.py          # Generate figures from reproduced results
└── plot_benchmarks_latex.py    # Generate figures from original results
```

Each `hsf/` and `nosvorig/` benchmark directory contains:
- Source code and Makefile
- `nosv.toml` — nOS-V runtime configuration
- `launcher_reproduce.sh` — SLURM launcher for reproduction
- `submit_reproduce.job` — SLURM batch script
- `README.md` — Benchmark description and HSF/baseline details

The `baseline/` benchmark directories contain only source code, a Makefile, and
the `launcher_reproduce.sh` / `submit_reproduce.job` pair — these are pure
OpenMP/MPI binaries with no nOS-V runtime, so there is no `nosv.toml`. They
provide the external (non-OmpSs-2) reference the figures normalize against
(`fox_cholesky_libflame`, `fox_heat_omp`, `fox_hpccg_omp`, `fox_mt-dgemm_libomp`,
`fox_multisaxpy_omp`). The benchmark Makefiles target the Fox architecture
explicitly (`-march=znver4`) so binaries are correct even when built off-cluster.

## Prerequisites

- **Nix** with flakes enabled
- **SLURM** cluster access (scripts target the Fox cluster with AMD EPYC 9684X Genoa-X)
- **Python 3** with `pandas`, `numpy`, `matplotlib` (for results collection and plotting)

## Reproducing results

### 1. Launch experiments

From the repository root, run the launchers for each benchmark and variant:

```bash
# HSF (taskgroup-optimized)
hsf/cholesky/launcher_reproduce.sh
hsf/heat/launcher_reproduce.sh
hsf/hpccg/launcher_reproduce.sh
hsf/matmul/launcher_reproduce.sh
hsf/multisaxpy/launcher_reproduce.sh

# OmpSs-2 baseline (nosvorig)
nosvorig/cholesky/launcher_reproduce.sh
nosvorig/heat/launcher_reproduce.sh
nosvorig/hpccg/launcher_reproduce.sh
nosvorig/matmul/launcher_reproduce.sh
nosvorig/multisaxpy/launcher_reproduce.sh

# External baseline (pure OpenMP/MPI)
baseline/cholesky/launcher_reproduce.sh
baseline/heat/launcher_reproduce.sh
baseline/hpccg/launcher_reproduce.sh
baseline/matmul/launcher_reproduce.sh
baseline/multisaxpy/launcher_reproduce.sh
```

Or submit everything at once with `bash launch_all.sh`.

Each launcher submits SLURM array jobs with 3 repetitions per configuration.
Output goes to `reproduced_results/fox_<benchmark>_<variant>/results_final_3bs/raw/`
(the `baseline/` launchers omit the `results_final_3bs/` level, writing directly
to `reproduced_results/fox_<benchmark>_<variant>/raw/`).

> **Note (cholesky external baseline):** the `baseline/cholesky` launcher
> reproduces n=6144 and n=33792 (the sizes used in the figures). n=49152 is
> omitted: its matrix exceeds 2³¹ elements and needs an ILP64 libFLAME, whereas
> the Nix flake provides LP64. mt-dgemm n=49152 is unaffected (BLIS uses 64-bit
> internal indices).

### 2. Collect results

After all jobs complete:

```bash
python collect_results.py
```

This parses SLURM output files and writes `summary.csv` files matching the format in `results/`.

### 3. Plot

```bash
python plot_reproduced.py --no-usetex    # fast preview without LaTeX
python plot_reproduced.py                 # LaTeX-rendered figures
```

Figures are written to `fig/` as PDF and PNG:
- `benchmark_comparison` — normalized bar chart (2x5 grid, large/small x 5 benchmarks)
- `benchmark_speedup_geomean` — geometric mean speedup per benchmark
- `benchmark_speedup_heatmap` — speedup heatmap across all configurations
- `benchmark_scaling_curves` — performance vs. task size

## Benchmarks

| Benchmark | Workload | HSF optimization |
|-----------|----------|-----------------|
| Cholesky | Dense Cholesky factorization | GEMM tasks grouped by tile column into CCD/NUMA taskgroups |
| Heat | 2D heat diffusion (Gauss-Seidel) | Diagonal wavefront with row-band taskgroups |
| HPCCG | Sparse CG solver (SpMV) | Matrix rows partitioned into CCD-level taskgroups |
| Matmul | Tiled matrix multiply | GEMM tasks grouped by output tile row |
| Multisaxpy | Repeated SAXPY on vector blocks | Round-robin block-to-core affinity with priority ordering |

See each benchmark's `README.md` for detailed descriptions of taskgroup policies, affinity mappings, and NUMA initialization strategies.
