# Multisaxpy -- Multiple SAXPY Operations -- HSF (Taskgroup-Optimized)

## What it computes

Repeated SAXPY (Y = alpha * X + Y) operations on vector blocks (same algorithm as baseline).

## Taskgroup optimizations

### Topology-aware affinity + intra-taskgroup priorities

Tasks are distributed across topology domains and prioritized:

```
tg_id = block_id % N_domains
priority = N_blocks - block_id
```

This ensures:
1. Blocks are spread evenly across topology domains (round-robin)
2. Within each domain, blocks are processed in ascending order (highest priority = lowest block_id)
3. When a block completes, the next iteration of the same block is likely processed by the same core, increasing cache reuse

### Affinity levels

- **core-level** (`lower=core`): Each core gets its own taskgroup. Maximum data reuse — consecutive iterations of the same block stay on the same core.
- **CCD-level** (`lower=cs`): Used for the largest tile size (TS=588800) where per-core affinity would create too many taskgroups.

### Measured cache improvements

The paper reports (vs. FIFO baseline):
- 68.97% increase in L2 cache hits
- 155.08% increase in L3 cache hits
- 63.99% reduction in L3 miss rate

### First-touch NUMA initialization

Initialization tasks are assigned to the same taskgroups as computation tasks:
```cpp
nosv_task_group_t tg = tglib_get_taskgroup_by_idx(blockid);
#pragma oss task taskgroup(tg) ...
init_block(x[blockid], y[blockid]);
```

This ensures data is physically placed on the NUMA node of the core that will process it.

### Task-to-data mapping

Each block `blockid` maps to `tg_id = blockid % N_domains`. Since blocks are contiguous memory regions, this creates a strided mapping: domain 0 gets blocks 0, N, 2N, ...; domain 1 gets blocks 1, N+1, 2N+1, etc. The priority ordering ensures temporal locality within each domain.

## How to build and run

```bash
cd hsf/multisaxpy
nix develop --impure ../
make
export VVV_TG_ENABLED=1 VVV_LOWER_LVL=core VVV_TG_POLICY=PRIO
./b6_multisaxpy_nodes 56524800 29440 1000
```

## Reproduce results

```bash
./launcher_reproduce.sh
cd ../..
python collect_results.py
```
