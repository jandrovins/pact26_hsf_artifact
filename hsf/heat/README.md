# Heat Equation Solver -- HSF (Taskgroup-Optimized)

## What it computes

2D heat diffusion solver using Gauss-Seidel iteration (same algorithm as baseline). Block-wise stencil computation with neighbor dependencies.

## Taskgroup optimizations

### Per-domain taskgroup mapping

One taskgroup is created per topology domain. Blocks are mapped to taskgroups by row index in round-robin:

```
tg_id = (row_index / DIAGS) % num_domains
```

With `DIAGS=1` (default), consecutive rows cycle through domains, creating a strided mapping that ensures each domain owns a balanced subset of the grid.

### Priority scheduling

Each taskgroup uses PRIO policy internally. The priority mode (`VVV_PRIORITY=0`) assigns priorities based on row position, encouraging wavefront progression across iterations.

### Affinity configuration

- **Small problem (N=12288)**: Strict CCD-level affinity (`lower=cs`, `upper=cs`, `flex=0`). All tasks in a taskgroup execute exclusively on the CCD to which the taskgroup is pinned, maximizing L3 cache reuse within a CCX cluster.
- **Large problem (N=49152)**: Flexible CCD-preferred / NUMA-acceptable (`lower=cs`, `upper=numa`, `flex=1`). Tasks prefer their CCD but can migrate within the NUMA node for load balancing.

### Task-to-data mapping

Each block (R, C) maps to a taskgroup via `getBlockIdx(R, C, ...)`. Since the same row-to-TG mapping is used across all iterations, the same block is always processed by the same topology domain, preserving cache locality for repeated stencil access.

## Memory allocation

Uses mmap with huge pages. First-touch from initialization tasks aligns NUMA placement with the taskgroup mapping.

## How to build and run

```bash
cd hsf/heat
nix develop --impure ../
make
export VVV_TG_ENABLED=1 VVV_LOWER_LVL=cs VVV_UPPER_LVL=cs VVV_AFF_FLEXIBLE=0
export VVV_TG_POLICY=PRIO VVV_DIAGS=1 VVV_PRIORITY=0 VVV_BLOCKFUNC=0
./02.heat_ompss2.bin -r 12288 -c 12288 -t 800 -b 256
```

## Reproduce results

```bash
./launcher_reproduce.sh
cd ../..
python collect_results.py
```
