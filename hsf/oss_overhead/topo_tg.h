#pragma once
#include <nosv.h>

/* Depth control constants */
#define TOPO_TG_LEVEL_NODE 0
#define TOPO_TG_LEVEL_NUMA 1
#define TOPO_TG_LEVEL_CS   2
#define TOPO_TG_LEVEL_CORE 3

/* 3-level hierarchy: NUMA -> CS -> Core */
void topo_tg_init(void);
nosv_task_group_t topo_tg_get(long i, long ntasks);

/* 4-level hierarchy: 3 Group TGs -> NUMA -> CS -> Core (full copy per group) */
void topo_tg_grp_init(void);
nosv_task_group_t topo_tg_grp_get(long i, long ntasks);

/* Configurable-depth 3-level hierarchy */
void topo_tg_init_level(int max_level);
nosv_task_group_t topo_tg_get_level(long i, long ntasks);
int topo_tg_num_leaf_domains(void);

/* Configurable-depth 4-level hierarchy (groups) */
void topo_tg_grp_init_level(int max_level);
nosv_task_group_t topo_tg_grp_get_level(long i, long ntasks);

/* Shared */
int topo_tg_num_cores(void);
int *topo_tg_get_prios(long ntasks);  /* random priorities array, or NULL */
