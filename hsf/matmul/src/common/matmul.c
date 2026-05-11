#include "matmul.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <tglib.h>
#include <nosv/hwinfo.h>

double matmul_gettime(void)
{
	struct timespec tv;
	clock_gettime(CLOCK_MONOTONIC, &tv);
	return tv.tv_sec+1e-9*tv.tv_nsec;
}

void matmul_fail(const char *msg)
{
	fprintf(stderr, "error: %s\n", msg);
	exit(1);
}

void matmul_getconf(int argc, char **argv, matmul_conf_t *conf, int nranks)
{
	if (argc != 6) {
		fprintf(stderr, "Usage: %s N M TIMESTEPS TILESIZE WARMUP\n", argv[0]);
		matmul_fail("missing parameters");
	}

	if (nranks <= 0)
		matmul_fail("nranks must be > 0");

	conf->n = (size_t)atoll(argv[1]);
	conf->m = (size_t)atoll(argv[2]);
	conf->timesteps = (size_t)atoi(argv[3]);
	conf->ts = (size_t)atoi(argv[4]);
	conf->warmup = (size_t)atoi(argv[5]);

	if (conf->ts == 0)
		matmul_fail("tile size must be > 0");

	/* Ensure m is divisible by nranks and that each rank's slice is divisible by the tile size.
	 * That is equivalent to requiring m % (nranks * ts) == 0. If not, round m up to the next multiple.
	 */
	const size_t block = (size_t)nranks * conf->ts;
	const size_t orig_m = conf->m;
	if (orig_m % block != 0) {
		const size_t new_m = ((orig_m + block - 1) / block) * block;
		fprintf(stderr, "Adjusting M from %zu to %zu so it is divisible by nranks*ts (%d*%zu=%zu)\n",
				orig_m, new_m, nranks, conf->ts, block);
		conf->m = new_m;
	}

	/* Ensure n is divisible by ts */
	if (conf->n % conf->ts != 0) {
		const size_t new_n = ((conf->n + conf->ts - 1) / conf->ts) * conf->ts;
		fprintf(stderr, "Adjusting N from %zu to %zu so it is divisible by ts (%zu)\n",
				conf->n, new_n, conf->ts);
		conf->n = new_n;
	}

	/* Final sanity checks (should pass after adjustment) */
	if (conf->m % (size_t)nranks != 0)
		matmul_fail("The matrix row size must be divisible by the number of processes");
	const size_t m_per_rank = conf->m / (size_t)nranks;
	if (conf->n % conf->ts != 0 || m_per_rank % conf->ts != 0)
		matmul_fail("The matrix size must be divisible by the tile size");
}

/* Parse a topology level name string (as used in VVV_LOWER_LVL / VVV_UPPER_LVL)
 * into a nosv_topo_level_t.  Accepted values: "node", "numa", "cs", "core", "cpu".
 * Returns NOSV_TOPO_LEVEL_COMPLEX_SET for any unrecognised string.
 */
static nosv_topo_level_t matmul_parse_topo_level(const char *s)
{
	if (!s || *s == '\0')           return NOSV_TOPO_LEVEL_COMPLEX_SET;
	if (strcmp(s, "node")  == 0)    return NOSV_TOPO_LEVEL_NODE;
	if (strcmp(s, "numa")  == 0)    return NOSV_TOPO_LEVEL_NUMA;
	if (strcmp(s, "cs")    == 0)    return NOSV_TOPO_LEVEL_COMPLEX_SET;
	if (strcmp(s, "core")  == 0)    return NOSV_TOPO_LEVEL_CORE;
	if (strcmp(s, "cpu")   == 0)    return NOSV_TOPO_LEVEL_CPU;
	fprintf(stderr, "warning: unknown topo level '%s', defaulting to cs\n", s);
	return NOSV_TOPO_LEVEL_COMPLEX_SET;
}

/* Compute the maximum square superblock side S such that the instantaneous
 * working set of a 2D block fits in L3:
 *   (S*S + S + S) * tile_bytes <= max_l3_bytes
 *   S^2 + 2S - max_tiles <= 0
 *   S = floor((-2 + sqrt(4 + 4*max_tiles)) / 2)
 */
static size_t matmul_compute_sb_side(size_t tile_bytes, size_t max_l3_bytes)
{
	size_t max_tiles = max_l3_bytes / tile_bytes;
	/* S^2 + 2S <= max_tiles  =>  S = floor((-2 + sqrt(4 + 4*max_tiles)) / 2) */
	double disc = 4.0 + 4.0 * (double)max_tiles;
	size_t S = (size_t)((-2.0 + sqrt(disc)) / 2.0);
	/* Clamp to at least 1 */
	if (S < 1) S = 1;
	return S;
}

/*
 * Find the largest divisor of nblocks that is <= max_side, so that the output
 * tile space can be partitioned into superblocks of equal size in that dimension.
 *
 * Equal-sized superblocks give every task group exactly the same number of
 * DGEMM tasks, avoiding the load imbalance that arises when a "runt" superblock
 * at the boundary receives only a fraction of the work of a full one.
 *
 * Fallback: if the best divisor found is < (2/3)*max_side (e.g. nblocks is prime
 * and the largest divisor below max_side is 1), the partition would be degenerate
 * (many 1-tile superblocks).  In that case we fall back to max_side and accept a
 * small remainder — the runt is unavoidable but max_side still gives the best L3
 * fit, and a single undersized boundary block is far less harmful than O(nblocks)
 * single-tile superblocks with no data reuse.
 */
static size_t matmul_find_even_side(size_t nblocks, size_t max_side)
{
	if (max_side >= nblocks)
		return nblocks;  /* entire dimension fits in one superblock */
	size_t best = 1;
	for (size_t d = max_side; d >= 1; --d) {
		if (nblocks % d == 0) {
			best = d;
			break;
		}
	}
	/* Pathological fallback: best divisor < 2/3 of max_side → revert to max_side */
	if (best * 3 < max_side * 2)
		return max_side;
	return best;
}

/*
 * Find superblock dimensions (sr, sc) that produce exactly 'target' superblocks
 * (ceil(nrowblocks/sr) * ceil(njblocks/sc) == target) with the working set
 * (sr*sc + sr + sc tiles) fitting in L3 cache.
 *
 * Among valid factorizations of 'target' into num_sb_rows * num_sb_cols, pick
 * the most balanced (sr ≈ sc) to maximize arithmetic intensity: for a fixed
 * number of output tiles, minimising sr + sc maximises flops per byte of A/B
 * traffic.
 *
 * Returns 1 on success, 0 if no valid factorization exists.
 */
static int matmul_find_sb_for_target(size_t nrowblocks, size_t njblocks,
                                      size_t target, size_t tile_bytes,
                                      size_t max_l3_bytes,
                                      size_t *out_sr, size_t *out_sc)
{
	size_t max_tiles = max_l3_bytes / tile_bytes;
	size_t best_sr = 0, best_sc = 0;
	size_t best_ws = SIZE_MAX;
	double best_ai = -1.0;  /* arithmetic intensity metric: sr*sc/(sr+sc) */

	for (size_t nr = 1; nr <= target && nr <= nrowblocks; ++nr) {
		if (target % nr != 0) continue;
		size_t nc = target / nr;
		if (nc > njblocks) continue;

		/* Compute sr for exactly nr superblock rows */
		size_t sr = (nrowblocks + nr - 1) / nr;
		if ((nrowblocks + sr - 1) / sr != nr) continue;  /* ceiling roundtrip check */

		/* Compute sc for exactly nc superblock columns */
		size_t sc = (njblocks + nc - 1) / nc;
		if ((njblocks + sc - 1) / sc != nc) continue;

		size_t ws = sr * sc + sr + sc;
		if (ws > max_tiles) continue;  /* doesn't fit in L3 */

		/* Prefer highest arithmetic intensity (most balanced sr/sc) */
		double ai = (double)(sr * sc) / (double)(sr + sc);
		if (ai > best_ai || (ai == best_ai && ws < best_ws)) {
			best_ai = ai;
			best_ws = ws;
			best_sr = sr;
			best_sc = sc;
		}
	}

	if (best_sr == 0) return 0;
	*out_sr = best_sr;
	*out_sc = best_sc;
	return 1;
}

void matmul_create_taskgroups(matmul_t *mm)
{
	tglib_init();

	size_t n = mm->n;
	size_t m = mm->m_per_rank;
	size_t ts = mm->ts;

	/* Check env var for hierarchy mode: 0 = flat (default), 1 = two-level */
	const char *hier_env = getenv("VVV_MATMUL_TG_HIERARCHY");
	mm->use_hierarchy = (hier_env && atoi(hier_env) == 1) ? 1 : 0;

	/* Calculate dimensions in blocks */
	size_t ncolblocks = n / ts;   /* k-dimension (inner product) */
	size_t nrowblocks = m / ts;   /* i-dimension (rows of A / rows of C per rank) */
	/* For the output matrix C, the j-dimension within a rank is also m_per_rank/ts */
	size_t njblocks = m / ts;     /* j-dimension */

	/* Size of one tile in bytes */
	size_t tile_size = ts * ts * sizeof(double);

	/* L3 cache limit per domain — configurable via VVV_L3_SIZE_MIB.
	 * Defaults: fox (AMD Zen4 CCX) = 96 MiB, owl (Intel Broadwell NUMA) = 35 MiB.
	 * Override at runtime: export VVV_L3_SIZE_MIB=<mib> */
	size_t l3_mib = 96;
	const char *l3_env = getenv("VVV_L3_SIZE_MIB");
	if (l3_env && atol(l3_env) > 0)
		l3_mib = (size_t)atol(l3_env);
	size_t max_l3_size = l3_mib * 1024 * 1024;
	fprintf(stderr, "  L3 domain size: %zu MiB (VVV_L3_SIZE_MIB=%s)\n",
			l3_mib, l3_env ? l3_env : "unset, default 96");

	/* Resolve topology levels from env vars (same names as tglib / launcher) */
	nosv_topo_level_t lower_lvl = matmul_parse_topo_level(getenv("VVV_LOWER_LVL"));
	nosv_topo_level_t upper_lvl = matmul_parse_topo_level(getenv("VVV_UPPER_LVL"));
	const char *flex_env = getenv("VVV_AFF_FLEXIBLE");
	int use_flexible = (flex_env && atoi(flex_env) == 1) ? 1 : 0;

	int num_doms = nosv_get_num_domains(lower_lvl);

	/* VVV_FORCE_NBLOCKS: when 1, force num_blocks == num_doms by finding a 2D
	 * factorization of num_doms that fits the tile grid and L3 cache.
	 * This ensures a 1:1 mapping between superblocks and hardware domains,
	 * eliminating idle domains when the L3-optimal blocking produces fewer
	 * blocks than available domains (e.g. 16 blocks vs 24 CCXs). */
	const char *force_env = getenv("VVV_FORCE_NBLOCKS");
	int force_nblocks = (force_env && atoi(force_env) == 1) ? 1 : 0;
	fprintf(stderr, "  Affinity tuning: force_nblocks=%d (VVV_FORCE_NBLOCKS=%s)\n",
			force_nblocks, force_env ? force_env : "unset, default 0");

	/* Compute 2D superblock dimensions.
	 * Default mode: matmul_find_even_side picks the largest divisor of the
	 * tile-grid dimension that fits within the L3 budget.
	 * Forced mode: matmul_find_sb_for_target finds sr/sc so that the
	 * superblock grid has exactly num_doms blocks (one per domain). */
	size_t S = matmul_compute_sb_side(tile_size, max_l3_size);
	size_t sr, sc;

	if (force_nblocks && num_doms > 1) {
		if (!matmul_find_sb_for_target(nrowblocks, njblocks,
				(size_t)num_doms, tile_size, max_l3_size, &sr, &sc)) {
			fprintf(stderr, "  Warning: cannot factor %d domains into a valid 2D grid "
					"for %zu x %zu tiles — falling back to L3-optimal sizing.\n",
					num_doms, nrowblocks, njblocks);
			sr = matmul_find_even_side(nrowblocks, S);
			sc = matmul_find_even_side(njblocks, S);
		} else {
			fprintf(stderr, "  Forced %d blocks (1:1 domain mapping): Sr=%zu, Sc=%zu\n",
					num_doms, sr, sc);
		}
	} else {
		sr = matmul_find_even_side(nrowblocks, S);
		sc = matmul_find_even_side(njblocks, S);
	}

	size_t num_sb_rows = (nrowblocks + sr - 1) / sr;
	size_t num_sb_cols = (njblocks + sc - 1) / sc;
	size_t num_blocks = num_sb_rows * num_sb_cols;

	/* Working set per 2D block: C block (sr*sc) + A column (sr) + B row (sc) tiles */
	size_t ws_tiles = sr * sc + sr + sc;
	double ws_mib = (double)(ws_tiles * tile_size) / (1024.0 * 1024.0);

	fprintf(stderr, "Matmul 2D Task Group Creation Report:\n");
	fprintf(stderr, "  Matrix tile grid: %zu rows x %zu cols (k-dim: %zu)\n",
			nrowblocks, njblocks, ncolblocks);
	fprintf(stderr, "  Tile size: %zu x %zu = %.2f KiB\n", ts, ts,
			(double)tile_size / 1024.0);
	/* L3 limit already printed above when reading VVV_L3_SIZE_MIB */
	const char *row_bal = (nrowblocks % sr == 0) ? "exact" : "runt (fallback)";
	const char *col_bal = (njblocks  % sc == 0) ? "exact" : "runt (fallback)";
	fprintf(stderr, "  2D superblock: Sr=%zu x Sc=%zu tiles (row balance: %s, col balance: %s)\n",
			sr, sc, row_bal, col_bal);
	fprintf(stderr, "  Working set per block: %zu tiles = %.2f MiB (C=%zu + A=%zu + B=%zu)\n",
			ws_tiles, ws_mib, sr * sc, sr, sc);
	fprintf(stderr, "  Superblock grid: %zu x %zu = %zu blocks\n",
			num_sb_rows, num_sb_cols, num_blocks);
	if (nrowblocks % sr != 0 || njblocks % sc != 0) {
		size_t runt_r = nrowblocks % sr ? nrowblocks % sr : sr;
		size_t runt_c = njblocks  % sc ? njblocks  % sc : sc;
		size_t runt_ws = runt_r * runt_c + runt_r + runt_c;
		double runt_work_pct = 100.0 * (double)(runt_r * runt_c) / (double)(sr * sc);
		fprintf(stderr, "  Boundary superblock: %zu x %zu tiles (%.1f%% of full block work)\n",
				runt_r, runt_c, runt_work_pct);
		(void)runt_ws;
	} else {
		fprintf(stderr, "  All superblocks are equal size — perfect load balance\n");
	}
	fprintf(stderr, "  Hierarchy mode: %s\n",
			mm->use_hierarchy ? "two-level (region -> block)" : "flat");

	mm->sr = sr;
	mm->sc = sc;
	mm->num_sb_rows = num_sb_rows;
	mm->num_sb_cols = num_sb_cols;
	mm->num_block_tgs = num_blocks;
	mm->block_tgs = malloc(num_blocks * sizeof(nosv_task_group_t));

	int *dom_ids = nosv_get_available_domains(lower_lvl);
	int *blocks_per_dom = calloc(num_doms, sizeof(int));

	fprintf(stderr, "  Affinity: lower=%s upper=%s flexible=%d force_nblocks=%d\n",
			getenv("VVV_LOWER_LVL") ? getenv("VVV_LOWER_LVL") : "cs",
			getenv("VVV_UPPER_LVL") ? getenv("VVV_UPPER_LVL") : "node",
			use_flexible, force_nblocks);
	fprintf(stderr, "  Available lower-level domains: %d\n", num_doms);

	if (mm->use_hierarchy) {
		/*
		 * Two-level hierarchy: Region TGs (one per lower-level domain) -> Block TGs.
		 * Region TGs carry the affinity; Block TGs are plain priority children.
		 */
		mm->num_region_tgs = (size_t)num_doms;
		mm->region_tgs = malloc(num_doms * sizeof(nosv_task_group_t));

		/* Create Region TGs — one per domain at the lower level */
		for (int d = 0; d < num_doms; ++d) {
			nosv_affinity_t aff;
			if (use_flexible)
				aff = nosv_affinity_get_flexible(upper_lvl, lower_lvl, (uint32_t)dom_ids[d]);
			else
				aff = nosv_affinity_get_strict(lower_lvl, (uint32_t)dom_ids[d]);
			char name[64];
			snprintf(name, sizeof(name), "Region_D%d", dom_ids[d]);
			nosv_task_group_create(&mm->region_tgs[d], NOSV_TG_PRIO_POLICY,
				NULL, &aff, num_doms - d, name, NOSV_TG_CREATE_NONE);
		}

		/* Create Block TGs as children of their region (no affinity, inherit) */
		for (size_t idx = 0; idx < num_blocks; ++idx) {
			size_t bi = idx / num_sb_cols;
			size_t bj = idx % num_sb_cols;
			int dom_idx = (int)(idx % (size_t)num_doms);
			blocks_per_dom[dom_idx]++;
			nosv_task_group_t parent = mm->region_tgs[dom_idx];

			char name[64];
			snprintf(name, sizeof(name), "B_%zu_%zu", bi, bj);
			nosv_task_group_create(&mm->block_tgs[idx], NOSV_TG_PRIO_POLICY,
				parent, NULL, (int)(num_blocks - idx), name, NOSV_TG_CREATE_NONE);
		}
	} else {
		/*
		 * Flat grid: each 2D block TG gets direct affinity at the lower level.
		 */
		mm->num_region_tgs = 0;
		mm->region_tgs = NULL;

		for (size_t idx = 0; idx < num_blocks; ++idx) {
			size_t bi = idx / num_sb_cols;
			size_t bj = idx % num_sb_cols;
			int dom_idx = (int)(idx % (size_t)num_doms);
			int dom_id = dom_ids[dom_idx];
			blocks_per_dom[dom_idx]++;

			nosv_affinity_t aff;
			if (use_flexible)
				aff = nosv_affinity_get_flexible(upper_lvl, lower_lvl, (uint32_t)dom_id);
			else
				aff = nosv_affinity_get_strict(lower_lvl, (uint32_t)dom_id);

			char name[64];
			snprintf(name, sizeof(name), "B_%zu_%zu", bi, bj);
			nosv_task_group_create(&mm->block_tgs[idx], NOSV_TG_PRIO_POLICY,
				NULL, &aff, (int)(num_blocks - idx), name, NOSV_TG_CREATE_NONE);
		}
	}

	if (rank == 0) {
		fprintf(stderr, "  Block distribution per lower-level domain:\n");
		for (int i = 0; i < num_doms; ++i) {
			fprintf(stderr, "    domain %d: %d blocks\n", dom_ids[i], blocks_per_dom[i]);
		}
	}
	free(blocks_per_dom);
	free(dom_ids);
}

static void matmul_fill_tile(size_t TS, double (*A)[TS], double value)
{
	double (* restrict A_block)[TS] = A;
	for (long ii = 0; ii < TS; ii++) {
		for (long jj = 0; jj < TS; jj++) {
			for (long kk = 0; kk < TS; kk++) {
				A_block[ii][jj] = value;
			}
		}
	}
}

void matmul_setup(matmul_conf_t *conf, matmul_t *matmul, size_t m_per_rank)
{
	const size_t n = conf->n;
	const size_t m = conf->m;
	const size_t ts = conf->ts;

	matmul->n = n;
	matmul->m = m;
	matmul->m_per_rank = m_per_rank;
	matmul->ts = ts;

	matmul_create_taskgroups(matmul);

	typedef double (*matrix_MxN_t)[n/ts][ts][ts];
	typedef double (*matrix_NxM_t)[m_per_rank/ts][ts][ts];

	matrix_MxN_t A, remote1, remote2;
	matrix_NxM_t B, C;

	size_t size_A = m_per_rank * n * sizeof(double);
	size_t size_B = n * m_per_rank * sizeof(double);
	size_t size_C = m * m_per_rank * sizeof(double);

	fprintf(stderr, "Allocating %.2f GiB of memory\n", (double)(size_A * 3 + size_B + size_C) / (1024.0 * 1024.0 * 1024.0));

	A = tglib_mmap_wrapper(size_A);
	B = tglib_mmap_wrapper(size_B);
	C = tglib_mmap_wrapper(size_C);
	remote1 = tglib_mmap_wrapper(size_A);
	remote2 = tglib_mmap_wrapper(size_A);

	if (!A || !B || !C || !remote1 || !remote2)
		matmul_fail("not enough memory");

	// Initialize matrices (first-block assignment for NUMA first-touch)
	// A[i][k]: assign to block (i_block, 0) — first column-block for row i
	for (size_t i = 0; i < m_per_rank/ts; ++i) {
		for (size_t k = 0; k < n/ts; ++k) {
			nosv_task_group_t tg = matmul_get_taskgroup(matmul, 0, i, 0);
			#pragma oss task out(A[i][k]) label("matmul_fill_tile A") taskgroup(tg)
			matmul_fill_tile(ts, A[i][k], 1.0);
		}
	}
	// B[k][j]: assign to block (0, j_block) — first row-block for column j
	for (size_t k = 0; k < n/ts; ++k) {
		for (size_t j = 0; j < m_per_rank/ts; ++j) {
			nosv_task_group_t tg = matmul_get_taskgroup(matmul, 0, 0, j);
			#pragma oss task out(B[k][j]) label("matmul_fill_tile B") taskgroup(tg)
			matmul_fill_tile(ts, B[k][j], 1.0);
		}
	}
	// C[i][j]: assign to its own 2D block — perfect placement
	for (size_t i = 0; i < m/ts; ++i) {
		for (size_t j = 0; j < m_per_rank/ts; ++j) {
			nosv_task_group_t tg = matmul_get_taskgroup(matmul, 0, i, j);
			#pragma oss task out(C[i][j]) label("matmul_fill_tile C") taskgroup(tg)
			matmul_fill_tile(ts, C[i][j], 0.0);
		}
	}
	#pragma oss taskwait

	matmul->A = (void *)A;
	matmul->remote1 = (void *)remote1;
	matmul->remote2 = (void *)remote2;
	matmul->B = (void *)B;
	matmul->C = (void *)C;
	matmul->alpha = 1.0;
	matmul->beta = 1.0;
}

int matmul_check(size_t N, size_t M, size_t TS, double (*A)[M/TS][TS][TS], double expected)
{
	int errors = 0;
	for (size_t R = 0; R < N/TS; ++R) {
		for (size_t C = 0; C < M/TS; ++C) {
			for (size_t r = 0; r < TS; ++r) {
				for (size_t c = 0; c < TS; ++c) {
					double value = A[R][C][r][c];
					if (fabs(value-expected) > 1e-5) {
						++errors;
						fprintf(stderr, "C[%ld][%ld][%ld][%ld]: %f, expected: %f\n", R, C, r, c, value, expected);
					}
				}
			}
		}
	}
	return errors;
}



static __uint128_t matmul_u128_checked_mul(__uint128_t a, __uint128_t b, const char *context)
{
	const __uint128_t max = ~(__uint128_t)0;
	if (a != 0 && b > max / a)
		matmul_fail(context);
	return a * b;
}
static long double matmul_compute_ops(const matmul_conf_t *conf)
{
	__uint128_t ops = (__uint128_t)conf->m;
	ops = matmul_u128_checked_mul(ops, (__uint128_t)conf->m, "operation count overflow");
	ops = matmul_u128_checked_mul(ops, (__uint128_t)conf->n, "operation count overflow");
	ops = matmul_u128_checked_mul(ops, (__uint128_t)conf->timesteps, "operation count overflow");
	ops = matmul_u128_checked_mul(ops, (__uint128_t)2, "operation count overflow");
	return (long double)ops;
}
void matmul_report(double t, matmul_conf_t *conf)
{
	const long double ops = matmul_compute_ops(conf);
	double performance = 0.0;
	if (t > 0.0)
		performance = (double)(ops / (1e9L * (long double)t));

	printf("Printing result %14e %zu %zu %zu %zu %14e\n",
			t,
			conf->n,
			conf->m,
			conf->timesteps,
			conf->ts,
			performance);
}
