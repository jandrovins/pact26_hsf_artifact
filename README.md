# HSF: A Hierarchical Scheduling Framework — Artifact

Artifact for the PACT'26 paper *HSF: A Hierarchical Scheduling Framework for
Application-Tailored Scheduling on Tasking Runtimes*.

- **Source / DOI:** GitHub `https://github.com/jandrovins/pact26_hsf_artifact` —
  archived on Zenodo at **DOI: [10.5281/zenodo.21877252](https://doi.org/10.5281/zenodo.21877252)**.
- **Target badges:** *Artifacts Available*, *Artifacts Evaluated — Functional*,
  and *Results Reproduced*.

HSF is a hierarchical scheduling framework built on two abstractions —
**predefined scheduling policies** and **taskgroups** (independent scheduling
domains) — integrated into the OmpSs-2 tasking model (nOS-V + NODES). It lets
each kernel/sub-computation run under a locally defined policy (FIFO, LIFO,
priority, affinity, and combinations), exposing affinity as a first-class knob
to trade data locality against load balance.

**Headline result:** across five benchmarks (Cholesky, Heat, HPCCG, Matmul,
Multisaxpy) at two problem scales, HSF's best configuration achieves a
**geometric-mean speedup of 1.84×** on large problems over the best
*OmpSs-2 + parallel-initialization* baseline, on a dual-socket AMD EPYC 9684X
(Genoa-X).

---

## Claims supported by this artifact

1. **HSF ≈1.84× geomean speedup on large problems** vs. the best OmpSs-2 +
   parallel-init configuration, across all five benchmarks. → regenerated as
   `fig/benchmark_speedup_geomean.pdf` / `benchmark_speedup_geomean_init.pdf`
   and the per-benchmark 2×5 grid `fig/benchmark_comparison.pdf`.
2. **Consistent per-benchmark speedups at both scales** (small and large) for
   Cholesky, Heat, HPCCG, Matmul and Multisaxpy. → `fig/benchmark_comparison.pdf`.
3. **Comparison against three baselines** — (a) a vendor OpenMP / BLAS-LAPACK
   reference (AMD BLIS/libFLAME or LLVM OpenMP, per benchmark), (b) unmodified
   OmpSs-2, and (c) OmpSs-2 + parallel initialization. → the `orig`, `init`,
   `tg` and external-baseline series in `fig/benchmark_comparison*.pdf`.
4. **Scheduling-overhead study across task granularities** ("Sched-Stress")
   comparing HSF against other runtimes. → the `oss_overhead` sweep and its
   plots.

## Claims *not* (fully) supported / conditional

- **Exact speedup magnitudes are hardware-specific.** They arise from the NUMA
  topology and stacked last-level cache (CCD) of the dual-socket AMD EPYC 9684X
  (Genoa-X). On other CPUs the artifact is *Functional* (it builds, runs, and
  produces the full pipeline output) but the 1.84× magnitude is **not** expected
  to reproduce. Faithful reproduction requires the target machine; reviewers can
  be given **remote SSH access** to a Genoa-X node (see the appendix).
- **`cholesky` external baseline at `n=49152`** is omitted: its matrix exceeds
  2³¹ elements and needs an ILP64 libFLAME, whereas the Nix flake ships LP64.
  `mt-dgemm n=49152` is unaffected (BLIS uses 64-bit internal indices).
- Absolute times depend on node, BIOS, and NUMA settings; only relative
  speedups are claimed.

---

## Dependencies

Everything is provisioned by **Nix flakes** (no manual dependency installation).
The pinned toolchain/runtimes are:

| Component | Source (public) | Pin |
|---|---|---|
| Nix package set | `NixOS/nixpkgs` (nixos-25.11) | `3c9db02…` |
| BSC Nix overlay | `github.com/jandrovins/jungle` (ref `tglib`) | `bb3e589…` |
| OmpSs-2 compiler (`clangOmpss2`) | `github.com/jandrovins/llvm-mono-ompss2` (branch `affinity_2`) | `3340e46…` |
| Taskgroup library (`tglib`) | `github.com/jandrovins/tglib` | `d78d0a4…` |
| nOS-V / NODES (baseline) | `bsc-pm/nos-v` 4.0.0 / `bsc-pm/nodes` 1.4.0 | release tarballs |
| nOS-V / NODES (HSF) | `jandrovins/nos-v-haffsched` / `jandrovins/nodes-haffsched` | `f6c19c40…` / `f090fc5c…` |
| Tracing (`ovni`) | `bsc-pm/ovni` | `275aea9…` |
| BLAS / LAPACK | AMD BLIS / libFLAME 5.1 (via overlay) | — |
| MPI | from nixpkgs | — |
| Plotting | Python 3 + pandas, numpy, matplotlib (in the dev shell) | — |

**Host requirements:** Linux x86-64 with **Nix (flakes enabled)**. Full
reproduction additionally needs a **SLURM** cluster and, for the paper's
numbers, a **dual-socket AMD EPYC 9684X (Genoa-X)** node (2×96 cores). The
Getting Started smoke test needs neither SLURM nor Genoa-X.

> **First build is long.** With no binary cache, `nix develop` compiles the
> OmpSs-2 LLVM (`clangOmpss2`), nOS-V, NODES and BLIS/libFLAME from source —
> potentially a few hours on the first invocation, then cached.

---

## Getting Started Guide (≈30 min, no SLURM)

1. **Install Nix with flakes.** See <https://nixos.org/download>. Enable flakes:
   ```bash
   mkdir -p ~/.config/nix && echo "experimental-features = nix-command flakes" >> ~/.config/nix/nix.conf
   ```
2. **Clone and enter the artifact.**
   ```bash
   git clone https://github.com/jandrovins/pact26_hsf_artifact.git
   cd pact26_hsf_artifact
   ```
3. **Run the smoke test.** This builds the whole toolchain (first time only)
   and runs one small Cholesky (N=6144) for both the OmpSs-2 baseline and HSF,
   directly on your local cores (no SLURM):
   ```bash
   bash run_local.sh              # or: CPUS=16 N=4096 TS=256 bash run_local.sh
   ```
   **Expected:** each variant prints a build log then a timing/GFLOP-s line
   ("Printing result: … "). Success = both runtimes build and run to completion.
   (On non-Genoa-X hardware the two times are not expected to match the paper.)

   Ignorable warnings: `NUMA interleaved subset nodes …`, `valloc: allocation
   mode = …`, `warning: unable to download … narinfo` (transient Nix cache),
   and `-march=native` compiler remarks.

---

## Step-by-Step Instructions (full reproduction)

Requires a SLURM cluster; the paper's magnitudes require a Genoa-X node.

### 1. Launch all experiments

```bash
# Faithful reproduction on AMD EPYC 9684X (Genoa-X):
HSF_ARCH=znver4 NREPS=3 bash launch_all.sh
```

`launch_all.sh` is configurable via environment variables (defaults in
parentheses): `PARTITION` (`fox`), `NREPS` (`3`), `NRUNS` (overhead sweep, `5`),
`HSF_ARCH` (`native`; use `znver4` on Genoa-X), and optional SLURM `ACCOUNT`,
`QOS`, `TIMELIMIT`. Examples:

```bash
PARTITION=myqueue ACCOUNT=myproj NREPS=5 HSF_ARCH=znver4 bash launch_all.sh
PARTITION=myqueue NREPS=1 bash launch_all.sh          # quick functional pass
```

Each launcher submits SLURM array jobs (`NREPS` reps per configuration).
Output goes to `reproduced_results/fox_<benchmark>_<variant>/results_final_3bs/raw/`
(the external `baseline/` launchers write to
`reproduced_results/fox_<benchmark>_<variant>/raw/`). Individual launchers can
also be run one at a time, e.g. `bash hsf/cholesky/launcher_reproduce.sh`.

**Runtime expectation:** the full sweep is hundreds of short array jobs; wall
time depends on queue occupancy. For a faster pass use `NREPS=1` and/or launch a
subset. To shrink the overhead sweep set `NRUNS=2`.

### 2. Collect results

After the jobs finish:

```bash
nix develop ./hsf --command python collect_results.py   # or any python3 w/ pandas
```

This parses the SLURM `*.out` files and writes a `summary.csv` under each
`reproduced_results/fox_<benchmark>_<variant>/…/` directory.

A **reference copy** of the authors' full Genoa-X run (`HSF_ARCH=znver4`,
`NREPS=3`) ships in `example_results/`, mirroring the `reproduced_results/`
layout and also including the `oss_overhead/` Sched-Stress sweep CSVs (under
`example_results/oss_overhead/{hsf,nosvorig}/`). Inspect it to see the expected
outputs, or regenerate the benchmark `summary.csv` files without a cluster run:

```bash
python collect_results.py --results-dir example_results
```

(`plot_reproduced.py` reads `reproduced_results/`; to plot the example data,
point it there, e.g. `cp -a example_results/* reproduced_results/`.)

### 3. Plot

```bash
python plot_reproduced.py --no-usetex   # fast preview (no LaTeX)
python plot_reproduced.py               # LaTeX-rendered figures
```

**Expected outputs** in `fig/` (PDF + PNG):
- `benchmark_comparison` — normalized 2×5 grid (large/small × 5 benchmarks).
- `benchmark_comparison_external_baseline` — normalized to the external baseline.
- `benchmark_speedup_geomean` — geometric-mean speedup per benchmark.
- `benchmark_speedup_geomean_init` — geomean vs. OmpSs-2 + parallel init
  (the ≈1.84× large-problem claim).

Compare the regenerated figures to the committed `fig/*` (the paper's figures).

---

## Repository structure

```
pact26_hsf_artifact/
├── hsf/                  # HSF-optimized benchmarks (taskgroup variant) + flake.nix
├── nosvorig/             # OmpSs-2 baselines (unmodified nOS-V 4.0.0) + flake.nix
├── baseline/             # External pure OpenMP/MPI (AMD BLIS/libFLAME) + flake.nix
│   └── <bench>/          # cholesky, heat, hpccg, matmul, multisaxpy
├── */oss_overhead/       # "Sched-Stress" task-overhead microbenchmark sweep
├── reproduced_results/   # Output tree (empty skeleton; populated by the run)
├── example_results/      # Reference copy of the authors' Genoa-X reproduction
├── fig/                  # Paper figures (regenerated by plot_reproduced.py)
├── launch_all.sh         # Submit every experiment (configurable)
├── run_local.sh          # SLURM-free getting-started smoke test
├── collect_results.py    # Parse SLURM output -> summary.csv
├── plot_reproduced.py    # Generate figures from reproduced results
└── ARTIFACT_APPENDIX.md  # ACM artifact appendix (checklist, install, evaluation)
```

Each `hsf/` and `nosvorig/` benchmark dir has its own `README.md` describing the
taskgroup policy, affinity mapping and NUMA initialization for that kernel.

## Benchmarks

| Benchmark | Workload | HSF optimization |
|-----------|----------|------------------|
| Cholesky | Dense Cholesky factorization | GEMM tasks grouped by tile column into CCD/NUMA taskgroups |
| Heat | 2D heat diffusion (Gauss-Seidel) | Diagonal wavefront with row-band taskgroups |
| HPCCG | Sparse CG solver (SpMV) | Matrix rows partitioned into CCD-level taskgroups |
| Matmul | Tiled matrix multiply | GEMM tasks grouped by output tile row |
| Multisaxpy | Repeated SAXPY on vector blocks | Round-robin block-to-core affinity with priority ordering |

See **`ARTIFACT_APPENDIX.md`** for the ACM artifact check-list, installation,
experiment workflow and evaluation details.
