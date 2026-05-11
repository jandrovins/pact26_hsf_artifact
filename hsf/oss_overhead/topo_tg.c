#include "topo_tg.h"
#include <nosv/hwinfo.h>
#include <nosv/affinity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Shared state ───────────────────────────────────────────────────────── */

static int n_numa, n_cs, n_cores, n_nodes;

/* Random-priority support */
static int  rand_prio_enabled = -1;   /* -1 = not checked yet */
static int *prio_array;
static long prio_ntasks;

/* ── 3-level hierarchy state ────────────────────────────────────────────── */

static int initialized;
static int tg_max_level;
static nosv_task_group_t node_tg;
static nosv_task_group_t *numa_tgs;
static nosv_task_group_t *cs_tgs;
static nosv_task_group_t *core_tgs;

/* ── 4-level hierarchy state ────────────────────────────────────────────── */

#define N_GROUPS 3

static int grp_initialized;
static int grp_max_level;
static nosv_task_group_t group_tgs[N_GROUPS];
static nosv_task_group_t *g_numa_tgs;
static nosv_task_group_t *g_cs_tgs;
static nosv_task_group_t *g_core_tgs;

/* ── Helpers ────────────────────────────────────────────────────────────── */

/*
 * find_parent_idx — given a child domain (level + SID), find which parent
 * domain (by logical index in parent_sids[]) contains the same CPUs.
 *
 * Method: grab one CPU from the child, then iterate parent domains and
 * check membership via nosv_get_available_cpus_in_domain().
 */
static int find_parent_idx(nosv_topo_level_t child_level, int child_sid,
                           nosv_topo_level_t parent_level,
                           int *parent_sids, int n_parent)
{
	int *child_cpus = nosv_get_available_cpus_in_domain(child_level, child_sid);
	int target_cpu = child_cpus[0];
	free(child_cpus);

	for (int j = 0; j < n_parent; j++) {
		int ncpus = nosv_get_num_cpus_in_domain(parent_level, parent_sids[j]);
		int *pcpus = nosv_get_available_cpus_in_domain(parent_level, parent_sids[j]);
		for (int k = 0; k < ncpus; k++) {
			if (pcpus[k] == target_cpu) {
				free(pcpus);
				return j;
			}
		}
		free(pcpus);
	}
	fprintf(stderr, "topo_tg: no parent for child_sid=%d\n", child_sid);
	exit(1);
}

static nosv_sched_policy_t get_policy(void)
{
	if (rand_prio_enabled == -1) {
		const char *env = getenv("VVV_RAND_PRIO");
		rand_prio_enabled = (env && atoi(env));
	}
	return rand_prio_enabled ? NOSV_TG_AFF_PRIO_POLICY : NOSV_TG_AFF_FIFO_POLICY;
}

/*
 * discover_topology — query nOS-V for domain counts and build
 * core→CS and CS→NUMA parent mappings.
 * Caller must free *out_core_to_cs and *out_cs_to_numa.
 */
static void discover_topology(int **out_core_to_cs, int **out_cs_to_numa)
{
	n_nodes = nosv_get_num_domains(NOSV_TOPO_LEVEL_NODE);
	n_numa  = nosv_get_num_domains(NOSV_TOPO_LEVEL_NUMA);
	n_cs    = nosv_get_num_domains(NOSV_TOPO_LEVEL_COMPLEX_SET);
	n_cores = nosv_get_num_domains(NOSV_TOPO_LEVEL_CORE);

	if (n_numa <= 0 || n_cs <= 0 || n_cores <= 0) {
		fprintf(stderr, "topo_tg: bad topology n_numa=%d n_cs=%d n_cores=%d\n",
		        n_numa, n_cs, n_cores);
		exit(1);
	}

	int *numa_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_NUMA);
	int *cs_sids   = nosv_get_available_domains(NOSV_TOPO_LEVEL_COMPLEX_SET);
	int *core_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_CORE);

	int *core_to_cs = malloc(n_cores * sizeof(int));
	int *cs_to_numa = malloc(n_cs    * sizeof(int));
	if (!core_to_cs || !cs_to_numa) { perror("malloc topo"); exit(1); }

	for (int i = 0; i < n_cores; i++)
		core_to_cs[i] = find_parent_idx(NOSV_TOPO_LEVEL_CORE, core_sids[i],
		                                NOSV_TOPO_LEVEL_COMPLEX_SET, cs_sids, n_cs);

	for (int i = 0; i < n_cs; i++)
		cs_to_numa[i] = find_parent_idx(NOSV_TOPO_LEVEL_COMPLEX_SET, cs_sids[i],
		                                NOSV_TOPO_LEVEL_NUMA, numa_sids, n_numa);

	free(numa_sids);
	free(cs_sids);
	free(core_sids);

	*out_core_to_cs = core_to_cs;
	*out_cs_to_numa = cs_to_numa;
}

/* ── Public: shared ─────────────────────────────────────────────────────── */

int topo_tg_num_cores(void) { return n_cores; }

int *topo_tg_get_prios(long ntasks)
{
	get_policy();   /* ensure rand_prio_enabled is set */
	if (!rand_prio_enabled) return NULL;
	if (prio_array) return prio_array;

	prio_array = malloc(ntasks * sizeof(int));
	if (!prio_array) { perror("malloc prios"); exit(1); }

	srand(42);
	for (long i = 0; i < ntasks; i++)
		prio_array[i] = rand();
	prio_ntasks = ntasks;
	return prio_array;
}

/* ── Public: 3-level hierarchy (NUMA → CS → Core) ──────────────────────── */

void topo_tg_init(void)
{
	if (initialized) return;
	initialized = 1;

	int *core_to_cs, *cs_to_numa;
	discover_topology(&core_to_cs, &cs_to_numa);

	int *numa_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_NUMA);
	int *cs_sids   = nosv_get_available_domains(NOSV_TOPO_LEVEL_COMPLEX_SET);
	int *core_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_CORE);

	nosv_sched_policy_t policy = get_policy();
	char label[64];
	int ret;

	/* NUMA TGs */
	numa_tgs = malloc(n_numa * sizeof *numa_tgs);
	for (int i = 0; i < n_numa; i++) {
		nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_NUMA, numa_sids[i]);
		snprintf(label, sizeof label, "numa-tg%d", numa_sids[i]);
		ret = nosv_task_group_create(&numa_tgs[i], policy, NULL,
		                             &aff, 0, label, 0);
		if (ret) { fprintf(stderr, "topo_tg: NUMA TG %d failed (%d)\n", i, ret); exit(1); }
	}

	/* CS TGs — parent = NUMA TG for this CS's NUMA node */
	cs_tgs = malloc(n_cs * sizeof *cs_tgs);
	for (int i = 0; i < n_cs; i++) {
		nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_COMPLEX_SET, cs_sids[i]);
		snprintf(label, sizeof label, "cs-tg%d", cs_sids[i]);
		ret = nosv_task_group_create(&cs_tgs[i], policy, numa_tgs[cs_to_numa[i]],
		                             &aff, 0, label, 0);
		if (ret) { fprintf(stderr, "topo_tg: CS TG %d failed (%d)\n", i, ret); exit(1); }
	}

	/* Core TGs — parent = CS TG for this core's CS */
	core_tgs = malloc(n_cores * sizeof *core_tgs);
	for (int i = 0; i < n_cores; i++) {
		nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_CORE, core_sids[i]);
		snprintf(label, sizeof label, "core-tg%d", core_sids[i]);
		ret = nosv_task_group_create(&core_tgs[i], policy, cs_tgs[core_to_cs[i]],
		                             &aff, 0, label, 0);
		if (ret) { fprintf(stderr, "topo_tg: Core TG %d failed (%d)\n", i, ret); exit(1); }
	}

	free(core_to_cs);
	free(cs_to_numa);
	free(numa_sids);
	free(cs_sids);
	free(core_sids);
}

nosv_task_group_t topo_tg_get(long i, long ntasks)
{
	long tasks_per_core = (ntasks + n_cores - 1) / n_cores;
	int core_idx = (int)(i / tasks_per_core);
	if (core_idx >= n_cores) core_idx = n_cores - 1;
	return core_tgs[core_idx];
}

/* ── Public: 4-level hierarchy (3 Groups → NUMA → CS → Core) ───────────── */

void topo_tg_grp_init(void)
{
	if (grp_initialized) return;
	grp_initialized = 1;

	int *core_to_cs, *cs_to_numa;
	discover_topology(&core_to_cs, &cs_to_numa);

	int *node_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_NODE);
	int *numa_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_NUMA);
	int *cs_sids   = nosv_get_available_domains(NOSV_TOPO_LEVEL_COMPLEX_SET);
	int *core_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_CORE);

	nosv_sched_policy_t policy = get_policy();
	char label[64];
	int ret;

	/* 3 Group TGs — strict NODE affinity */
	for (int g = 0; g < N_GROUPS; g++) {
		nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_NODE, node_sids[0]);
		snprintf(label, sizeof label, "grp-tg%d", g);
		ret = nosv_task_group_create(&group_tgs[g], policy, NULL,
		                             &aff, 0, label, 0);
		if (ret) { fprintf(stderr, "topo_tg: Group TG %d failed (%d)\n", g, ret); exit(1); }
	}

	/* Per-group: full copy of NUMA → CS → Core subtree */
	g_numa_tgs = malloc(N_GROUPS * n_numa  * sizeof *g_numa_tgs);
	g_cs_tgs   = malloc(N_GROUPS * n_cs    * sizeof *g_cs_tgs);
	g_core_tgs = malloc(N_GROUPS * n_cores * sizeof *g_core_tgs);
	if (!g_numa_tgs || !g_cs_tgs || !g_core_tgs) { perror("malloc grp tgs"); exit(1); }

	for (int g = 0; g < N_GROUPS; g++) {
		for (int i = 0; i < n_numa; i++) {
			nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_NUMA, numa_sids[i]);
			snprintf(label, sizeof label, "g%d-numa-tg%d", g, numa_sids[i]);
			ret = nosv_task_group_create(&g_numa_tgs[g * n_numa + i], policy,
			                             group_tgs[g],
			                             &aff, 0, label, 0);
			if (ret) { fprintf(stderr, "topo_tg: g%d numa TG %d failed (%d)\n", g, i, ret); exit(1); }
		}

		for (int i = 0; i < n_cs; i++) {
			nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_COMPLEX_SET, cs_sids[i]);
			snprintf(label, sizeof label, "g%d-cs-tg%d", g, cs_sids[i]);
			ret = nosv_task_group_create(&g_cs_tgs[g * n_cs + i], policy,
			                             g_numa_tgs[g * n_numa + cs_to_numa[i]],
			                             &aff, 0, label, 0);
			if (ret) { fprintf(stderr, "topo_tg: g%d cs TG %d failed (%d)\n", g, i, ret); exit(1); }
		}

		for (int i = 0; i < n_cores; i++) {
			nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_CORE, core_sids[i]);
			snprintf(label, sizeof label, "g%d-core-tg%d", g, core_sids[i]);
			ret = nosv_task_group_create(&g_core_tgs[g * n_cores + i], policy,
			                             g_cs_tgs[g * n_cs + core_to_cs[i]],
			                             &aff, 0, label, 0);
			if (ret) { fprintf(stderr, "topo_tg: g%d core TG %d failed (%d)\n", g, i, ret); exit(1); }
		}
	}

	free(core_to_cs);
	free(cs_to_numa);
	free(node_sids);
	free(numa_sids);
	free(cs_sids);
	free(core_sids);
}

nosv_task_group_t topo_tg_grp_get(long i, long ntasks)
{
	long tasks_per_group = (ntasks + N_GROUPS - 1) / N_GROUPS;
	int group = (int)(i / tasks_per_group);
	if (group >= N_GROUPS) group = N_GROUPS - 1;

	long group_start = (long)group * tasks_per_group;
	long group_size  = (group < N_GROUPS - 1) ? tasks_per_group : ntasks - group_start;
	long local_i     = i - group_start;

	long tasks_per_core = (group_size + n_cores - 1) / n_cores;
	int core_idx = (int)(local_i / tasks_per_core);
	if (core_idx >= n_cores) core_idx = n_cores - 1;

	return g_core_tgs[group * n_cores + core_idx];
}

/* ── Public: configurable-depth 3-level hierarchy ──────────────────────── */

void topo_tg_init_level(int max_level)
{
	if (initialized) return;
	initialized = 1;
	tg_max_level = max_level;

	int *core_to_cs, *cs_to_numa;
	discover_topology(&core_to_cs, &cs_to_numa);

	int *node_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_NODE);
	int *numa_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_NUMA);
	int *cs_sids   = nosv_get_available_domains(NOSV_TOPO_LEVEL_COMPLEX_SET);
	int *core_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_CORE);

	nosv_sched_policy_t policy = get_policy();
	char label[64];
	int ret;

	if (max_level == TOPO_TG_LEVEL_NODE) {
		/* Single node-level TG */
		nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_NODE, node_sids[0]);
		snprintf(label, sizeof label, "node-tg%d", node_sids[0]);
		ret = nosv_task_group_create(&node_tg, policy, NULL, &aff, 0, label, 0);
		if (ret) { fprintf(stderr, "topo_tg: Node TG failed (%d)\n", ret); exit(1); }
		goto done;
	}

	/* NUMA TGs */
	numa_tgs = malloc(n_numa * sizeof *numa_tgs);
	for (int i = 0; i < n_numa; i++) {
		nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_NUMA, numa_sids[i]);
		snprintf(label, sizeof label, "numa-tg%d", numa_sids[i]);
		ret = nosv_task_group_create(&numa_tgs[i], policy, NULL,
		                             &aff, 0, label, 0);
		if (ret) { fprintf(stderr, "topo_tg: NUMA TG %d failed (%d)\n", i, ret); exit(1); }
	}
	if (max_level == TOPO_TG_LEVEL_NUMA) goto done;

	/* CS TGs */
	cs_tgs = malloc(n_cs * sizeof *cs_tgs);
	for (int i = 0; i < n_cs; i++) {
		nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_COMPLEX_SET, cs_sids[i]);
		snprintf(label, sizeof label, "cs-tg%d", cs_sids[i]);
		ret = nosv_task_group_create(&cs_tgs[i], policy, numa_tgs[cs_to_numa[i]],
		                             &aff, 0, label, 0);
		if (ret) { fprintf(stderr, "topo_tg: CS TG %d failed (%d)\n", i, ret); exit(1); }
	}
	if (max_level == TOPO_TG_LEVEL_CS) goto done;

	/* Core TGs */
	core_tgs = malloc(n_cores * sizeof *core_tgs);
	for (int i = 0; i < n_cores; i++) {
		nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_CORE, core_sids[i]);
		snprintf(label, sizeof label, "core-tg%d", core_sids[i]);
		ret = nosv_task_group_create(&core_tgs[i], policy, cs_tgs[core_to_cs[i]],
		                             &aff, 0, label, 0);
		if (ret) { fprintf(stderr, "topo_tg: Core TG %d failed (%d)\n", i, ret); exit(1); }
	}

done:
	free(core_to_cs);
	free(cs_to_numa);
	free(node_sids);
	free(numa_sids);
	free(cs_sids);
	free(core_sids);
}

nosv_task_group_t topo_tg_get_level(long i, long ntasks)
{
	int n_leaf;
	nosv_task_group_t *leaf_tgs;

	switch (tg_max_level) {
	case TOPO_TG_LEVEL_NODE:
		return node_tg;
	case TOPO_TG_LEVEL_NUMA:
		n_leaf = n_numa;  leaf_tgs = numa_tgs; break;
	case TOPO_TG_LEVEL_CS:
		n_leaf = n_cs;    leaf_tgs = cs_tgs;   break;
	default: /* CORE */
		n_leaf = n_cores; leaf_tgs = core_tgs; break;
	}

	long tasks_per_domain = (ntasks + n_leaf - 1) / n_leaf;
	int idx = (int)(i / tasks_per_domain);
	if (idx >= n_leaf) idx = n_leaf - 1;
	return leaf_tgs[idx];
}

int topo_tg_num_leaf_domains(void)
{
	switch (tg_max_level) {
	case TOPO_TG_LEVEL_NODE: return n_nodes;
	case TOPO_TG_LEVEL_NUMA: return n_numa;
	case TOPO_TG_LEVEL_CS:   return n_cs;
	default:                  return n_cores;
	}
}

/* ── Public: configurable-depth 4-level hierarchy (groups) ─────────────── */

void topo_tg_grp_init_level(int max_level)
{
	if (grp_initialized) return;
	grp_initialized = 1;
	grp_max_level = max_level;

	int *core_to_cs, *cs_to_numa;
	discover_topology(&core_to_cs, &cs_to_numa);

	int *node_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_NODE);
	int *numa_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_NUMA);
	int *cs_sids   = nosv_get_available_domains(NOSV_TOPO_LEVEL_COMPLEX_SET);
	int *core_sids = nosv_get_available_domains(NOSV_TOPO_LEVEL_CORE);

	nosv_sched_policy_t policy = get_policy();
	char label[64];
	int ret;

	/* 3 Group TGs — strict NODE affinity */
	for (int g = 0; g < N_GROUPS; g++) {
		nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_NODE, node_sids[0]);
		snprintf(label, sizeof label, "grp-tg%d", g);
		ret = nosv_task_group_create(&group_tgs[g], policy, NULL,
		                             &aff, 0, label, 0);
		if (ret) { fprintf(stderr, "topo_tg: Group TG %d failed (%d)\n", g, ret); exit(1); }
	}

	if (max_level == TOPO_TG_LEVEL_NODE) goto done;

	/* Per-group NUMA TGs */
	g_numa_tgs = malloc(N_GROUPS * n_numa * sizeof *g_numa_tgs);
	if (!g_numa_tgs) { perror("malloc grp numa tgs"); exit(1); }

	for (int g = 0; g < N_GROUPS; g++) {
		for (int i = 0; i < n_numa; i++) {
			nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_NUMA, numa_sids[i]);
			snprintf(label, sizeof label, "g%d-numa-tg%d", g, numa_sids[i]);
			ret = nosv_task_group_create(&g_numa_tgs[g * n_numa + i], policy,
			                             group_tgs[g], &aff, 0, label, 0);
			if (ret) { fprintf(stderr, "topo_tg: g%d numa TG %d failed (%d)\n", g, i, ret); exit(1); }
		}
	}
	if (max_level == TOPO_TG_LEVEL_NUMA) goto done;

	/* Per-group CS TGs */
	g_cs_tgs = malloc(N_GROUPS * n_cs * sizeof *g_cs_tgs);
	if (!g_cs_tgs) { perror("malloc grp cs tgs"); exit(1); }

	for (int g = 0; g < N_GROUPS; g++) {
		for (int i = 0; i < n_cs; i++) {
			nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_COMPLEX_SET, cs_sids[i]);
			snprintf(label, sizeof label, "g%d-cs-tg%d", g, cs_sids[i]);
			ret = nosv_task_group_create(&g_cs_tgs[g * n_cs + i], policy,
			                             g_numa_tgs[g * n_numa + cs_to_numa[i]],
			                             &aff, 0, label, 0);
			if (ret) { fprintf(stderr, "topo_tg: g%d cs TG %d failed (%d)\n", g, i, ret); exit(1); }
		}
	}
	if (max_level == TOPO_TG_LEVEL_CS) goto done;

	/* Per-group Core TGs */
	g_core_tgs = malloc(N_GROUPS * n_cores * sizeof *g_core_tgs);
	if (!g_core_tgs) { perror("malloc grp core tgs"); exit(1); }

	for (int g = 0; g < N_GROUPS; g++) {
		for (int i = 0; i < n_cores; i++) {
			nosv_affinity_t aff = nosv_affinity_get_flexible(NOSV_TOPO_LEVEL_NODE, NOSV_TOPO_LEVEL_CORE, core_sids[i]);
			snprintf(label, sizeof label, "g%d-core-tg%d", g, core_sids[i]);
			ret = nosv_task_group_create(&g_core_tgs[g * n_cores + i], policy,
			                             g_cs_tgs[g * n_cs + core_to_cs[i]],
			                             &aff, 0, label, 0);
			if (ret) { fprintf(stderr, "topo_tg: g%d core TG %d failed (%d)\n", g, i, ret); exit(1); }
		}
	}

done:
	free(core_to_cs);
	free(cs_to_numa);
	free(node_sids);
	free(numa_sids);
	free(cs_sids);
	free(core_sids);
}

nosv_task_group_t topo_tg_grp_get_level(long i, long ntasks)
{
	long tasks_per_group = (ntasks + N_GROUPS - 1) / N_GROUPS;
	int group = (int)(i / tasks_per_group);
	if (group >= N_GROUPS) group = N_GROUPS - 1;

	if (grp_max_level == TOPO_TG_LEVEL_NODE)
		return group_tgs[group];

	long group_start = (long)group * tasks_per_group;
	long group_size  = (group < N_GROUPS - 1) ? tasks_per_group : ntasks - group_start;
	long local_i     = i - group_start;

	int n_leaf;
	nosv_task_group_t *leaf_tgs;

	switch (grp_max_level) {
	case TOPO_TG_LEVEL_NUMA:
		n_leaf = n_numa;  leaf_tgs = g_numa_tgs + group * n_numa;  break;
	case TOPO_TG_LEVEL_CS:
		n_leaf = n_cs;    leaf_tgs = g_cs_tgs   + group * n_cs;    break;
	default: /* CORE */
		n_leaf = n_cores; leaf_tgs = g_core_tgs + group * n_cores; break;
	}

	long tasks_per_domain = (group_size + n_leaf - 1) / n_leaf;
	int idx = (int)(local_i / tasks_per_domain);
	if (idx >= n_leaf) idx = n_leaf - 1;
	return leaf_tgs[idx];
}
