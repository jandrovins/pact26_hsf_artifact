#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <numaif.h>
#include <ovni.h>


#ifdef USE_MKL
#include <mkl.h>
#else
#include <cblas.h>
#include <lapacke.h>
#endif

#include <tglib.h>

#include <nosv.h>
#include <nosv/hwinfo.h>
#include <nosv/affinity.h>

#define CHECK_NOSV_CHOL(f...)                                                           \
do {                                                                               \
	const int __r = f;                                                             \
	if (__r) {                                                                     \
		fprintf(stderr, "Error: '%s' [%s:%i]: %i\n", #f, __FILE__, __LINE__, __r); \
		abort();                                                                    \
	}                                                                              \
} while (0)

#define PRIME1 293
#define PRIME2 719

long n = 4L * 1024L;
long ts = 1024L;
long nblocks_per_dim = -1;

tglib_tg_array_t chol_tgs;

/* ========================================
 * GEMM-specific Task Group Pool
 * Separate FIFO TG pool for gemm tasks.
 * Consecutive gemm tasks (in creation order) are batched into the same TG.
 * Counter resets each Cholesky iteration k.
 * Controlled by VVV_CHOL_GEMM_TILES_PER_BLOCK env variable.
 * ======================================== */

static nosv_task_group_t *gemm_tg_pool = NULL;
static long gemm_pool_size = 0;
static long gemm_tiles_per_block = 48; // Default: 48 tiles per batch (fits ~96 MiB at TS=512)
static long gemm_counter = 0;         // Per-iteration counter, reset at each k
// Default OFF: GEMMs use the column round-robin taskgroups (like potrf/trsm/syrk).
// Set VVV_CHOL_GEMM_TG=1 to opt into the separate FIFO GEMM-blocking pool.
static int gemm_tg_enabled = 0;

//static void cholesky_init_taskgroups(void)
//{
//	nblocks_per_dim = (n + ts - 1) / ts; 
//	long ntiles = nblocks_per_dim * nblocks_per_dim;
//	//tglib_create_taskgroups_strict_lazy(ntiles);
//	tglib_create_taskgroups_strict_rr(ntiles);
//}
//static inline nosv_task_group_t cholesky_get_taskgroup(long brow, long bcol)
//{
//	if (brow > nblocks_per_dim || bcol > nblocks_per_dim) {
//		fprintf(stderr, "Error: block indices out of range in cholesky_get_taskgroup\n");
//		abort();
//	}
//	long idx = brow * nblocks_per_dim + bcol;
//	return tglib_get_taskgroup_by_idx(idx);
//}
typedef enum {
	ROW_TYPE = 22,
	COL_TYPE = 23,
	K_TYPE = 24
} chol_mark_type_t;

int ovni_enabled = 0;
void init_marks()
{
	const char *config_override = getenv("NOSV_CONFIG_OVERRIDE");
	if (config_override && strstr(config_override, "ovni")) {
		ovni_enabled = 1;
	}
	if (!ovni_enabled)
		return;

	ovni_mark_type(ROW_TYPE, OVNI_MARK_STACK, "CHOL_ROW");
	ovni_mark_type(COL_TYPE, OVNI_MARK_STACK, "CHOL_COL");
	ovni_mark_type(K_TYPE, OVNI_MARK_STACK, "CHOL_K");
}

/* Helper: convert string to nOS-V topology level */
static nosv_topo_level_t chol_string_to_topo_level(const char *s)
{
	if (strcmp(s, "node") == 0) return NOSV_TOPO_LEVEL_NODE;
	if (strcmp(s, "numa") == 0) return NOSV_TOPO_LEVEL_NUMA;
	if (strcmp(s, "cs") == 0)   return NOSV_TOPO_LEVEL_COMPLEX_SET;
	if (strcmp(s, "core") == 0) return NOSV_TOPO_LEVEL_CORE;
	if (strcmp(s, "cpu") == 0)  return NOSV_TOPO_LEVEL_CPU;
	fprintf(stderr, "Invalid topology level: %s\n", s);
	abort();
}

static void cholesky_init_gemm_pool(void)
{
	/* GEMM blocking is opt-in. By default GEMMs use the column round-robin TGs. */
	const char *env_gtg = getenv("VVV_CHOL_GEMM_TG");
	gemm_tg_enabled = (env_gtg && strcmp(env_gtg, "1") == 0) ? 1 : 0;
	if (!gemm_tg_enabled) {
		printf("=== GEMM TG Pool: DISABLED (VVV_CHOL_GEMM_TG!=1) ===\n");
		printf("  GEMMs use the column round-robin taskgroups (by output column).\n");
		printf("====================\n");
		return;
	}

	/* Read tiles-per-block from environment */
	const char *env_tpb = getenv("VVV_CHOL_GEMM_TILES_PER_BLOCK");
	if (env_tpb) {
		gemm_tiles_per_block = atol(env_tpb);
		if (gemm_tiles_per_block <= 0) {
			fprintf(stderr, "Invalid VVV_CHOL_GEMM_TILES_PER_BLOCK: %s. Must be > 0.\n", env_tpb);
			abort();
		}
	}

	/* Pool size: enough TGs to cover the largest iteration's gemm tasks (k=0) */
	/* Iteration k=0 has B*(B-1)/2 gemm tasks. Pool = ceil(that / tiles_per_block) */
	long max_gemm_per_iter = nblocks_per_dim * (nblocks_per_dim - 1) / 2;
	gemm_pool_size = (max_gemm_per_iter + gemm_tiles_per_block - 1) / gemm_tiles_per_block;
	if (gemm_pool_size <= 0) gemm_pool_size = 1;

	/* Read affinity topology levels (reuse tglib's VVV_LOWER_LVL / VVV_UPPER_LVL) */
	const char *env_lower = getenv("VVV_LOWER_LVL");
	nosv_topo_level_t lower = env_lower ? chol_string_to_topo_level(env_lower) : NOSV_TOPO_LEVEL_COMPLEX_SET;
	const char *env_upper = getenv("VVV_UPPER_LVL");
	nosv_topo_level_t upper = env_upper ? chol_string_to_topo_level(env_upper) : NOSV_TOPO_LEVEL_NUMA;

	int flexible = 0;
	const char *env_flex = getenv("VVV_AFF_FLEXIBLE");
	if (env_flex && strcmp(env_flex, "1") == 0) flexible = 1;

	/* Get available hardware domains */
	long num_doms = nosv_get_num_domains(lower);
	if (num_doms <= 0) {
		fprintf(stderr, "Invalid nosv_get_num_domains(%d): %ld\n", (int)lower, num_doms);
		abort();
	}
	int *available_doms = nosv_get_available_domains(lower);
	if (!available_doms) {
		fprintf(stderr, "nosv_get_available_domains(%d) returned NULL\n", (int)lower);
		abort();
	}

	/* Allocate and create the gemm TG pool */
	gemm_tg_pool = (nosv_task_group_t *)malloc(sizeof(nosv_task_group_t) * gemm_pool_size);
	if (!gemm_tg_pool) {
		fprintf(stderr, "Failed to allocate gemm_tg_pool\n");
		abort();
	}

	for (long i = 0; i < gemm_pool_size; i++) {
		nosv_affinity_t tg_aff;
		if (flexible) {
			tg_aff = nosv_affinity_get_flexible(upper, lower, available_doms[i % num_doms]);
		} else {
			tg_aff = nosv_affinity_get_strict(lower, available_doms[i % num_doms]);
		}

		char affinity_str[256];
		nosv_affinity_to_string(tg_aff, affinity_str, sizeof(affinity_str));

		char tg_name[512];
		snprintf(tg_name, sizeof(tg_name), "gemm_pool_aff=%s_idx=%ld", affinity_str, i);

		/* FIFO policy — preserves creation-order temporal locality */
		CHECK_NOSV_CHOL(nosv_task_group_create(&gemm_tg_pool[i],
			NOSV_TG_FIFO_POLICY, NULL, &tg_aff, 0, tg_name,
			NOSV_TG_CREATE_NONE));
	}

	free(available_doms);

	/* Print gemm pool info */
	double batch_mib = (double)(gemm_tiles_per_block * ts * ts * sizeof(double)) / (1024.0 * 1024.0);
	printf("=== GEMM TG Pool ===\n");
	printf("  VVV_CHOL_GEMM_TILES_PER_BLOCK = %ld\n", gemm_tiles_per_block);
	printf("  Max gemm tasks/iter (k=0)     = %ld\n", max_gemm_per_iter);
	printf("  Pool size                     = %ld TGs\n", gemm_pool_size);
	printf("  Batch output working set      = %.1f MiB (L3 = 96 MiB)\n", batch_mib);
	printf("  Policy                        = FIFO\n");
	printf("  Affinity                      = %s (lower=%s upper=%s)\n",
	        flexible ? "flexible" : "strict",
	        env_lower ? env_lower : "cs",
	        env_upper ? env_upper : "numa");
	printf("====================\n");
}

static void cholesky_destroy_gemm_pool(void)
{
	if (gemm_tg_pool) {
		for (long i = 0; i < gemm_pool_size; i++) {
			if (gemm_tg_pool[i]) {
				CHECK_NOSV_CHOL(nosv_task_group_destroy(gemm_tg_pool[i]));
			}
		}
		free(gemm_tg_pool);
		gemm_tg_pool = NULL;
	}
	gemm_pool_size = 0;
}

static void cholesky_init_taskgroups(void)
{
	init_marks();
	nblocks_per_dim = (n + ts - 1) / ts;

	/* Create per-domain TGs for potrf/trsm/syrk (PRIO, column round-robin) */
	tglib_create_taskgroups_per_domain();
	chol_tgs = tglib_get_taskgroup_array();

	printf("=== Column RR TG Mapping (potrf/trsm/syrk) ===\n");
	printf("  nblocks_per_dim = %ld\n", nblocks_per_dim);
	printf("  Num TGs         = %ld\n", chol_tgs.num_tgs);
	printf("  Policy          = PRIO\n");
	printf("================================================\n");

	/* Create separate FIFO gemm TG pool */
	cholesky_init_gemm_pool();
}

/* Column round-robin for potrf/trsm/syrk/init */
static inline nosv_task_group_t cholesky_get_taskgroup(long brow, long bcol)
{
	if (chol_tgs.num_tgs == 0) {
		return NULL;
	}

	if (brow > nblocks_per_dim || bcol > nblocks_per_dim) {
		fprintf(stderr, "Error: block indices out of range in cholesky_get_taskgroup\n");
		fprintf(stderr, "brow: %ld, bcol: %ld, nblocks_per_dim: %ld\n", brow, bcol, nblocks_per_dim);
		abort();
	}

	return chol_tgs.tgs[bcol % chol_tgs.num_tgs];
}

/* Batched counter-based mapping for gemm tasks.
 * Called sequentially during task creation; counter resets each iteration k.
 * Every gemm_tiles_per_block consecutive gemm tasks share the same TG. */
static inline nosv_task_group_t cholesky_get_gemm_taskgroup(void)
{
	if (!gemm_tg_pool || gemm_pool_size == 0) {
		return NULL;
	}

	long tg_idx = (gemm_counter / gemm_tiles_per_block) % gemm_pool_size;
	gemm_counter++;
	return gemm_tg_pool[tg_idx];
}

/* GEMM taskgroup selector: the separate FIFO pool when opted in
 * (VVV_CHOL_GEMM_TG=1), otherwise the default column round-robin (by output
 * column j), so GEMMs are scheduled like potrf/trsm/syrk. */
static inline nosv_task_group_t cholesky_gemm_tg(long i, long j)
{
	if (gemm_tg_enabled)
		return cholesky_get_gemm_taskgroup();
	return cholesky_get_taskgroup(i, j);
}

static int check_gemm_prio_overflow(long N, long TS)
{
	long nblocks = N / TS;
	
	// The maximum value of gemm_prio occurs when k = 0, j = k+1 = 1, and i is at its maximum (nblocks-1)
	// gemm_prio = 1e4*(nblocks_per_dim - k) + (1e3)*(nblocks_per_dim - j) + i
	// Max case: k = 0, j = 1, i = nblocks - 1
	long max_k_term = (long)(1e4 * nblocks);
	long max_j_term = (long)(1e3 * (nblocks - 1));
	long max_i_term = nblocks - 1;
	long max_gemm_prio = max_k_term + max_j_term + max_i_term;
	
	printf("=== GEMM Priority Overflow Check ===\n");
	printf("Matrix size N = %ld, Tile size TS = %ld\n", N, TS);
	printf("Number of blocks per dimension: %ld\n", nblocks);
	printf("Maximum k term (1e4 * nblocks): %ld\n", max_k_term);
	printf("Maximum j term (1e3 * (nblocks - 1)): %ld\n", max_j_term);
	printf("Maximum i term (nblocks - 1): %ld\n", max_i_term);
	printf("Maximum gemm_prio value: %ld\n", max_gemm_prio);
	printf("INT_MAX: %d\n", INT_MAX);
	
	if (max_gemm_prio > INT_MAX) {
		fprintf(stderr, "ERROR: gemm_prio will overflow!\n");
		fprintf(stderr, "  Maximum possible value: %ld\n", max_gemm_prio);
		fprintf(stderr, "  INT_MAX: %d\n", INT_MAX);
		fprintf(stderr, "  Overflow amount: %ld\n", max_gemm_prio - INT_MAX);
		fprintf(stderr, "Consider reducing the priority coefficients or using a larger integer type.\n");
		return -1;
	}
	
	printf("INFO: No overflow detected. gemm_prio is safe.\n");
	printf("====================================\n\n");
	return 0;
}

static void cholesky_orig(long N, long TS, double (*A)[N/TS][TS][TS])
{
	// IDEA: tener un taskgroup for iteration k, and all tasks in that iteration use that taskgroup. Set taskgruop priority to N/TS - k
	// Then, one taskgroup per row R, where all tasks working on that row use that taskgroup. Set priority of this taskgroup to nrows - R,
	// so that rows at the top of the matrix have higher priority
	for (long k = 0; k < N/TS; k++) {
		#pragma oss task inout(A[k][k]) label("potrf")
		LAPACKE_dpotrf(LAPACK_ROW_MAJOR, 'L', TS, (double *) A[k][k], TS);
		
		for (long i = k+1; i < N/TS; i++) {
			#pragma oss task in(A[k][k]) inout(A[i][k]) label("trsm")
			cblas_dtrsm(CblasRowMajor, CblasRight, CblasLower, CblasTrans, CblasNonUnit, TS, TS, 1.0, (double const *) A[k][k], TS, (double *) A[i][k], TS);
		}
		
		for (long i = k+1; i < N/TS; i++) {
			for (long j = k+1; j < i; j++) {
				#pragma oss task in(A[i][k], A[j][k]) inout(A[i][j]) label("gemm")
				{
					if (ovni_enabled) {
						ovni_mark_push(ROW_TYPE, i+1);
						ovni_mark_push(COL_TYPE, j+1);
						ovni_mark_push(K_TYPE, k+1);
					}
				cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasTrans, TS, TS, TS, -1.0, (double const *) A[i][k], TS, (double const *) A[j][k], TS, 1.0, (double *) A[i][j], TS);
					if (ovni_enabled) {
						ovni_mark_pop(ROW_TYPE, i+1);
						ovni_mark_pop(COL_TYPE, j+1);
						ovni_mark_pop(K_TYPE, k+1);
					}
				}
			}
			#pragma oss task in(A[i][k]) inout(A[i][i]) label("syrk")
			cblas_dsyrk(CblasRowMajor, CblasLower, CblasNoTrans, TS, TS, -1.0, (double const *) A[i][k], TS, 1.0, (double *) A[i][i], TS);
		}
	}
}

static void cholesky(long N, long TS, double (*A)[N/TS][TS][TS], int use_priority)
{
	int prio_syrk = use_priority ? 10000 : 0;
	int prio_potrf = use_priority ? 20000 : 0;
	int prio_trsm = use_priority ? 30000 : 0;

	for (long k = 0; k < N/TS; k++) {
		/* Reset gemm counter each iteration for temporal locality within each k */
		gemm_counter = 0;

		int potrf_prio = use_priority ? ((nblocks_per_dim - k) + prio_potrf) : 0;
		#pragma oss task priority(potrf_prio) taskgroup(cholesky_get_taskgroup(k, k)) inout(A[k][k]) label("potrf")
		LAPACKE_dpotrf(LAPACK_ROW_MAJOR, 'L', TS, (double *) A[k][k], TS);
		
		for (long i = k+1; i < N/TS; i++) {
			int prio = use_priority ? (nblocks_per_dim - i + prio_trsm) : 0;
			#pragma oss task priority(prio) taskgroup(cholesky_get_taskgroup(i, k)) in(A[k][k]) inout(A[i][k]) label("trsm")
			cblas_dtrsm(CblasRowMajor, CblasRight, CblasLower, CblasTrans, CblasNonUnit, TS, TS, 1.0, (double const *) A[k][k], TS, (double *) A[i][k], TS);
		}
		
		for (long i = k+1; i < N/TS; i++) {
			for (long j = k+1; j < i; j++) {
				/* GEMM taskgroup: column round-robin by default, FIFO pool if opted in */
				#pragma oss task taskgroup(cholesky_gemm_tg(i, j)) in(A[i][k], A[j][k]) inout(A[i][j]) label("gemm")
				{
					if (ovni_enabled) {
						ovni_mark_push(ROW_TYPE, i+1);
						ovni_mark_push(COL_TYPE, j+1);
						ovni_mark_push(K_TYPE, k+1);
					}
				cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasTrans, TS, TS, TS, -1.0, (double const *) A[i][k], TS, (double const *) A[j][k], TS, 1.0, (double *) A[i][j], TS);
					if (ovni_enabled) {
						ovni_mark_pop(ROW_TYPE, i+1);
						ovni_mark_pop(COL_TYPE, j+1);
						ovni_mark_pop(K_TYPE, k+1);
					}
				}
			}
			int prio = use_priority ? (nblocks_per_dim - i + prio_syrk) : 0;
			#pragma oss task priority(prio) taskgroup(cholesky_get_taskgroup(i, i)) in(A[i][k]) inout(A[i][i]) label("syrk")
			cblas_dsyrk(CblasRowMajor, CblasLower, CblasNoTrans, TS, TS, -1.0, (double const *) A[i][k], TS, 1.0, (double *) A[i][i], TS);
		}
	}
}

static void initialize(long N, long TS, double (*a)[N/TS][TS][TS])
{

	for (long i = 0; i < N; i += TS) {
		for (long j = 0; j < N; j += TS) {
			#pragma oss task taskgroup(cholesky_get_taskgroup(i/TS, j/TS)) label("init_tile")
			{
				for (long ii = i; ii < i + TS; ii++) {
					for (long jj = j; jj < j + TS; jj++) {
						// Generate a value that makes the matrix symmetric positive definite
						double value = (((ii+jj) % PRIME1) + 1) * (((ii+jj) % PRIME2) + 1);
						if (ii == jj) {
							value += PRIME1 * PRIME2;
						}

						// Tiled layout
						a[ii/TS][jj/TS][ii%TS][jj%TS] = value;
					}
				}
				// Since we are using lazy strict affinity for taskgroups, give some time for 
				// other tasks to start on other cores, so that work is better distributed
				usleep(100);
			}
		}
	}

	// Print NUMA location of pages of buffer a
	{
		long total_bytes = sizeof(double) * N * N;
		long page_size = sysconf(_SC_PAGESIZE);
		long num_pages = (total_bytes + page_size - 1) / page_size;
		void **pages = (void **) malloc(num_pages * sizeof(void *));
		int *status = (int *) malloc(num_pages * sizeof(int));

		for (long p = 0; p < num_pages; p++) {
			pages[p] = (char *)a + p * page_size;
		}

		int ret = move_pages(0, num_pages, pages, NULL, status, 0);
		if (ret != 0) {
			perror("move_pages failed");
		} else {
			// Count pages per NUMA node
			int max_node = 0;
			for (long p = 0; p < num_pages; p++) {
				if (status[p] >= 0 && status[p] > max_node)
					max_node = status[p];
			}
			long *node_count = (long *) calloc(max_node + 2, sizeof(long));
			long unmapped_count = 0;
			for (long p = 0; p < num_pages; p++) {
				if (status[p] >= 0)
					node_count[status[p]]++;
				else
					unmapped_count++;
			}
			printf("NUMA page distribution for matrix a (%ld pages, page_size=%ld):\n", num_pages, page_size);
			for (int nd = 0; nd <= max_node; nd++) {
				if (node_count[nd] > 0)
					printf("  NUMA node %d: %ld pages (%.2f%%)\n", nd, node_count[nd], 100.0 * node_count[nd] / num_pages);
			}
			if (unmapped_count > 0)
				printf("  Unmapped/error: %ld pages\n", unmapped_count);
			free(node_count);
		}

		free(pages);
		free(status);
	}

	#pragma oss taskwait
}

int main(int argc, char **argv)
{
	tglib_init();

	int use_priority = 1;
	const char *vvv_priority_enabled = getenv("VVV_CHOL_PRIORITY_ENABLED");
	if (vvv_priority_enabled && strcmp(vvv_priority_enabled, "0") == 0) {
		use_priority = 0;
	}
	printf("VVV_CHOL_PRIORITY_ENABLED is set to %d\n", use_priority);

	if (argc > 4 || (argc >= 2 && strcmp(argv[1], "-h") == 0)) {
		fprintf(stderr, "[USAGE] %s N tasksize\n", argv[0]);
		fprintf(stderr, "  aaa tasksize must divide N\n");
		return 1;
	}

	if (argc >= 2)
		n = atol(argv[1]);

	if (argc >= 3)
		ts = atol(argv[2]);

	long iterations = 1;
	if (argc >= 4)
		iterations = atol(argv[3]);

	if (n % ts != 0) {
		long old_n = n;
		n = ((n + ts - 1) / ts) * ts;
		fprintf(stderr, "Warning: N (%ld) is not a multiple of tasksize (%ld). Adjusted N to %ld.\n", old_n, ts, n);
	}

	typedef double (*matrix_t)[n/ts][ts][ts];

	//matrix_t a = (matrix_t) malloc(sizeof(double) * n * n);
	matrix_t a = tglib_mmap_wrapper(sizeof(double) * n * n);
	printf("Matrix size: %ld bytes\n", sizeof(double) * n * n);
	printf("Matrix dimensions: %ld x %ld\n", n, n);

	cholesky_init_taskgroups();

	// Check for gemm_prio overflow before running cholesky
	if (check_gemm_prio_overflow(n, ts) != 0) {
		fprintf(stderr, "Aborting due to potential overflow in gemm_prio calculation.\n");
		tglib_destroy();
		return 1;
	}

	// Warmup iteration
	initialize(n, ts, a);
	struct timespec warmup_start, warmup_end;
	clock_gettime(CLOCK_MONOTONIC, &warmup_start);

	//cholesky(n, ts, a);

	#pragma oss taskwait

	clock_gettime(CLOCK_MONOTONIC, &warmup_end);

	double warmup_duration = (warmup_end.tv_sec - warmup_start.tv_sec) * 1000000 + ((double)(warmup_end.tv_nsec - warmup_start.tv_nsec)) / 1000.0;
	warmup_duration /= 1000000;

	printf("Warmup cholesky duration: %14e seconds\n", warmup_duration);

	int tg_enabled = 0;
	const char *vvv_tg_enabled = getenv("VVV_TG_ENABLED");
	if (vvv_tg_enabled && strcmp(vvv_tg_enabled, "1") == 0) {
		tg_enabled = 1;
	}

	if (!tg_enabled)
		printf("VVV_TG_ENABLED is not set to 1, running cholesky_orig\n");
	else
		printf("VVV_TG_ENABLED is set to 1, running cholesky\n");

	// Real initialization
	initialize(n, ts, a);

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (int i = 0; i < iterations; i++) {
		cholesky(n, ts, a, use_priority);
	}

	#pragma oss taskwait

	clock_gettime(CLOCK_MONOTONIC, &end);

	double duration = (end.tv_sec - start.tv_sec) * 1000000 + ((double)(end.tv_nsec - start.tv_nsec)) / 1000.0;
	duration /= 1000000;

	// GFlops
	double performance = (0.33*n*n*n + 0.5*n*n + 0.17*n) * iterations;
	performance = performance / duration;
	performance /= 1000000000;

	printf("Printing result: ");
	printf("%14e %14e %14ld %14ld\n", duration, performance, n, ts);

	cholesky_destroy_gemm_pool();
	tglib_destroy();

	return 0;
}
