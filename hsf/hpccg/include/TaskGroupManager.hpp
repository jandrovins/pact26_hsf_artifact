#pragma once

// TaskGroupManager — thin wrapper around tglib for HPCCG
//
// The main TG pool (for waxpby, ddot, init, exchange) is created via
// tglib_create_taskgroups_per_domain(), controlled by VVV_* env vars
// (VVV_TG_ENABLED, VVV_LOWER_LVL, VVV_UPPER_LVL, VVV_AFF_FLEXIBLE,
// VVV_TG_POLICY).
//
// An optional second TG pool optimized for HPC_sparsemv tasks can be
// enabled via VVV_SPMV_TG_ENABLED=1. This pool uses:
//   - FIFO policy by default (temporal/spatial locality for sequential
//     row access to the sparse matrix)
//   - Flexible affinity (prefer CCX, accept NUMA) for load balance
//   - Immediate-successor mode to chain sparsemv tasks on the same CPU
//
// New env vars for the sparsemv pool:
//   VVV_SPMV_TG_ENABLED  0|1   (default 0)
//   VVV_SPMV_POLICY       PRIO|FIFO|LIFO  (default FIFO)
//   VVV_SPMV_FLEXIBLE     0|1   (default 1)
//   VVV_SPMV_IMM          0|1   (default 1)

#include <nosv.h>
#include <nosv/hwinfo.h>
#include <nosv/affinity.h>
#include <tglib.h>

#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>

class TaskGroupManager {
private:
	long int nrows;

	// Sparsemv-specific TG pool
	nosv_task_group_t *spmv_tgs;
	int num_spmv_tgs;
	int nrows_per_spmv_tg;
	bool spmv_pool_enabled;

	static nosv_topo_level_t parse_topo_level(const char *s, nosv_topo_level_t def) {
		if (!s) return def;
		if (strcmp(s, "node") == 0) return NOSV_TOPO_LEVEL_NODE;
		if (strcmp(s, "numa") == 0) return NOSV_TOPO_LEVEL_NUMA;
		if (strcmp(s, "cs")   == 0) return NOSV_TOPO_LEVEL_COMPLEX_SET;
		if (strcmp(s, "core") == 0) return NOSV_TOPO_LEVEL_CORE;
		if (strcmp(s, "cpu")  == 0) return NOSV_TOPO_LEVEL_CPU;
		return def;
	}

	void create_spmv_pool() {
		const char *env = std::getenv("VVV_SPMV_TG_ENABLED");
		if (!env || strcmp(env, "1") != 0) {
			spmv_pool_enabled = false;
			std::cerr << "VVV_SPMV_TG_ENABLED=0: sparsemv reuses main TG pool" << std::endl;
			return;
		}
		spmv_pool_enabled = true;

		// Scheduling policy — FIFO preserves creation (= row) order,
		// maximizing sequential access to matrix data in L3
		nosv_sched_policy_t policy = NOSV_TG_FIFO_POLICY;
		const char *env_pol = std::getenv("VVV_SPMV_POLICY");
		if (env_pol) {
			if (strcmp(env_pol, "PRIO") == 0) policy = NOSV_TG_PRIO_POLICY;
			else if (strcmp(env_pol, "LIFO") == 0) policy = NOSV_TG_LIFO_POLICY;
		}

		// Flexible affinity — let sparsemv tasks migrate to idle CPUs
		// within the NUMA node when their home CCX is overloaded
		bool flexible = true;
		const char *env_flex = std::getenv("VVV_SPMV_FLEXIBLE");
		if (env_flex && strcmp(env_flex, "0") == 0) flexible = false;

		// Immediate-successor mode — when a sparsemv task completes,
		// the next ready task in the same TG starts on the same CPU,
		// keeping matrix data and output vector hot in L1/L2
		bool imm = true;
		const char *env_imm = std::getenv("VVV_SPMV_IMM");
		if (env_imm && strcmp(env_imm, "0") == 0) imm = false;

		nosv_topo_level_t lower = parse_topo_level(
			std::getenv("VVV_LOWER_LVL"), NOSV_TOPO_LEVEL_COMPLEX_SET);
		nosv_topo_level_t upper = parse_topo_level(
			std::getenv("VVV_UPPER_LVL"), NOSV_TOPO_LEVEL_NUMA);

		int num_doms = nosv_get_num_domains(lower);
		int *doms = nosv_get_available_domains(lower);
		if (num_doms <= 0 || !doms) {
			std::cerr << "Warning: cannot create spmv TG pool" << std::endl;
			spmv_pool_enabled = false;
			return;
		}

		int spmv_mult = 1;
		const char *env_mult = std::getenv("VVV_SPMV_TG_MULT");
		if (env_mult) spmv_mult = std::max(1, atoi(env_mult));
		num_spmv_tgs = num_doms * spmv_mult;
		nrows_per_spmv_tg = (nrows + num_spmv_tgs - 1) / num_spmv_tgs;
		spmv_tgs = (nosv_task_group_t *)malloc(
			sizeof(nosv_task_group_t) * num_spmv_tgs);

		nosv_flags_t flags = NOSV_TG_CREATE_NONE;
		if (imm) flags |= NOSV_TG_CREATE_ENABLE_IMM;

		for (int i = 0; i < num_spmv_tgs; i++) {
			// Block distribution: consecutive TGs map to the same domain,
			// aligned with first-touch initialization via get_row_taskgroup().
			int dom_idx = i / spmv_mult;
			if (dom_idx >= num_doms) dom_idx = num_doms - 1;

			nosv_affinity_t aff;
			if (flexible)
				aff = nosv_affinity_get_flexible(upper, lower, doms[dom_idx]);
			else
				aff = nosv_affinity_get_strict(lower, doms[dom_idx]);

			char label[128];
			snprintf(label, sizeof(label), "spmv_tg_%d", i);

			int err = nosv_task_group_create(
				&spmv_tgs[i], policy, NULL, &aff,
				num_spmv_tgs - i, label, flags);
			if (err) {
				fprintf(stderr, "nosv_task_group_create failed for spmv TG %d: %d\n", i, err);
				abort();
			}
		}
		free(doms);

		std::cerr << "Created " << num_spmv_tgs << " sparsemv TGs"
				  << " (" << num_doms << " domains x " << spmv_mult << " mult"
				  << ", policy=" << (policy == NOSV_TG_FIFO_POLICY ? "FIFO" :
									 policy == NOSV_TG_PRIO_POLICY ? "PRIO" : "LIFO")
				  << ", flex=" << flexible
				  << ", imm=" << imm << ")" << std::endl;
	}

public:
	TaskGroupManager()
		: nrows(0), spmv_tgs(nullptr), num_spmv_tgs(0),
		  nrows_per_spmv_tg(0), spmv_pool_enabled(false) {}

	/// Must be called before tglib_destroy() to safely destroy spmv TGs
	/// while nOS-V is still running. The destructor is a no-op after this.
	void cleanup() {
		if (spmv_tgs) {
			for (int i = 0; i < num_spmv_tgs; i++)
				if (spmv_tgs[i]) nosv_task_group_destroy(spmv_tgs[i]);
			free(spmv_tgs);
			spmv_tgs = nullptr;
		}
	}

	~TaskGroupManager() {
		// nosv_task_group_destroy must not be called after tglib_destroy().
		// cleanup() must be called explicitly before tglib_destroy() in main.
		if (spmv_tgs) {
			// Safety net: if cleanup() was not called, free the memory at least.
			free(spmv_tgs);
			spmv_tgs = nullptr;
		}
	}

	/// Called once from generate_matrix after tglib_init().
	/// Creates the main TG pool via tglib plus the optional spmv pool.
	void create_taskgroups(const int arg_nrows) {
		nrows = arg_nrows;
		tglib_create_taskgroups_per_domain();
		tglib_tg_array_t arr = tglib_get_taskgroup_array();
		std::cerr << "tglib: " << arr.num_tgs
				  << " main TGs for " << nrows << " rows" << std::endl;
		create_spmv_pool();
	}

	/// Row-based linear-partition lookup — for waxpby, ddot, init, MPI.
	nosv_task_group_t get_row_taskgroup(const int row) {
		return tglib_get_taskgroup_linear(row, nrows);
	}

	/// Round-robin lookup — for exchange_externals.
	nosv_task_group_t get_flat_taskgroup(const int idx) {
		return tglib_get_taskgroup_by_idx(idx);
	}

	/// Sparsemv-optimized lookup — returns from the FIFO+flexible pool
	/// when VVV_SPMV_TG_ENABLED=1, otherwise falls back to main pool.
	nosv_task_group_t get_spmv_taskgroup(const int row) {
		if (spmv_pool_enabled) {
			int tg_id = std::min(row / nrows_per_spmv_tg,
								num_spmv_tgs - 1);
			return spmv_tgs[tg_id];
		}
		return get_row_taskgroup(row);
	}
};

extern TaskGroupManager tg_manager;
