# Matrix Multiplication -- HSF (Taskgroup-Optimized)

## What it computes

Tiled dense matrix multiplication C = A * B using BLAS DGEMM (same algorithm as baseline).

## Taskgroup optimizations

### 2D superblock partitioning

The output tile space (C matrix) is partitioned into a 2D grid of superblocks. Each DGEMM task is assigned to the taskgroup that owns its output tile:

```
tg_id = superblock_of(output_tile_row, output_tile_col)
```

Superblock dimensions are sized so the instantaneous working set (input tiles from A and B, output tile from C) fits within the L3 cache of the assigned topology domain (`VVV_L3_SIZE_MIB=96` for Fox Genoa-X).

### Affinity configuration

Per-tile-size tuning based on working set analysis:

| N | TS | lower | upper | flex | Rationale |
|---|---|---|---|---|---|
| 6144 | 192,256,512 | cs | cs | 0 | Small: strict CCD, everything fits in L3 |
| 49152 | 192 | cs | numa | 1 | Fine tiles: flexible, allow NUMA load balancing |
| 49152 | 256 | numa | numa | 0 | Medium tiles: strict NUMA |
| 49152 | 512 | numa | node | 1 | Large tiles: flexible, allow cross-NUMA |

### Task-to-data mapping

Each DGEMM task computing C[i][j] += A[i][k] * B[k][j] is assigned to `tg_id = superblock(i, j)`. The superblock grid ensures that tasks writing to nearby output tiles execute on the same topology domain, maximizing output tile reuse in cache.

`VVV_FORCE_NBLOCKS=1` forces the taskgroup count to match the number of topology domains at the chosen level.

## Memory allocation

Uses mmap with `VVV_MMAP_ENABLED=1`. Initialization tasks are assigned to matching taskgroups for first-touch NUMA placement.

## How to build and run

```bash
cd hsf/matmul
nix develop --impure ../
make
export VVV_TG_ENABLED=1 VVV_LOWER_LVL=cs VVV_UPPER_LVL=cs VVV_AFF_FLEXIBLE=0
export VVV_FORCE_NBLOCKS=1 VVV_L3_SIZE_MIB=96
./build/02.matmul_ompss2_itampi.bin 6144 6144 20 256 1
```

## Reproduce results

```bash
./launcher_reproduce.sh
cd ../..
python collect_results.py
```
