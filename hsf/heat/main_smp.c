#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ovni.h>
#include <nosv.h>
#include <tglib.h>

#include "heat.h"

double progStartTime = -1000000;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
extern bool is_debug;
extern bool ovni_enabled;
void check_debug_enabled();
void check_ovni_enabled();
void initialize(HeatConfiguration *conf, int64_t rows, int64_t cols, int64_t rowOffset);

static inline void computeBlock(const int64_t rows, const int64_t cols,
		const int rstart, const int rend,
		const int cstart, const int cend,
		double M[restrict rows][cols]);
// Debug printing helper
bool ovni_enabled = false;
bool is_debug = false;

void check_debug_enabled() {
	const char* debug_env = getenv("VVV_DEBUG");
	is_debug = debug_env && strcmp(debug_env, "1") == 0;
}

enum {
	MARK_TYPE_IT = 22,
	MARK_TYPE_ROW = 23,
	MARK_TYPE_COL = 24,
	MARK_TYPE_ROW_PLUS_COL = 25,
};
void check_ovni_enabled() {
	const char* vvv_ovni_env = getenv("VVV_OVNI");
	ovni_enabled = vvv_ovni_env && strcmp(vvv_ovni_env, "1") == 0;
	if (is_debug)
		printf("OVNI enabled: %s\n", ovni_enabled ? "true" : "false");

	if (ovni_enabled) {
		ovni_mark_type(MARK_TYPE_ROW, OVNI_MARK_STACK, "MARKS FOR ROW ID");
		ovni_mark_type(MARK_TYPE_COL, OVNI_MARK_STACK, "MARKS FOR COL ID");
		ovni_mark_type(MARK_TYPE_ROW_PLUS_COL, OVNI_MARK_STACK, "MARKS FOR ROW + COL");
		ovni_mark_type(MARK_TYPE_IT, OVNI_MARK_STACK, "MARKS FOR ITERATION");
	}
}
// Debug printing helper
bool is_debug_enabled() {
	return is_debug;
}

const char *
summary(void)
{
	return "Parallel version using OmpSs-2 tasks";
}

int DIAGS = 15;
int PRIORITY_MODE = 0;
int BLOCKFUNC_MODE = 0;
int submit_window_enabled = 0;

// Number of task groups (filled after tglib_create_taskgroups_per_domain())
static int ndoms = 0;

// Calculate priority based on the selected mode
int calculatePriority(int R, int C, int it, int maxIt, int nrb, int ncb) {
	switch (PRIORITY_MODE) {
		case 0:
			return maxIt - it;
		case 1:
			return R;
		case 2:
			return R + C;
		case 3:
			return (R + C) * 10000 + R;
		case 4:
			return (nrb - R) + (ncb - C);
		default:
			return 0; // Default priority
	}
}

// Returns a task group index for the given block position.
// BLOCKFUNC_MODE=0 (row):      idx = (R / DIAGS) % ntgs
// BLOCKFUNC_MODE=1 (diagonal): idx = ((R+C) / DIAGS) % ntgs
// BLOCKFUNC_MODE=2 (linear):   idx = (R * ntgs) / (nrb - 2)
int getBlockIdx(int R, int C, int nrb, int ncb)
{
	(void)ncb;
	if (ndoms == 0) {
		fprintf(stderr, "%s: Error: No domains available. Please check the affinity level and available domains.\n", __func__);
		abort();
	}

	int ret;
	switch (BLOCKFUNC_MODE) {
		case 0:
			ret = ((R / DIAGS) % ndoms);
			break;
		case 1:
			ret = ((R + C) / DIAGS) % ndoms;
			break;
		case 2:
			ret = (R * ndoms) / (nrb - 2);
			break;
		default:
			ret = ((R / DIAGS) % ndoms); // Default to mode 0
			break;
	}

	return ret;
}

static inline void gaussSeidelSolver(int64_t rows, int64_t cols, int rbs, int cbs, int nrb, int ncb, double M[rows][cols], char reps[nrb][ncb], int it, int maxIt)
{
	for (int R = 1; R < nrb-1; ++R) {
		for (int C = 1; C < ncb-1; ++C) {
			nosv_task_group_t tg = tglib_get_taskgroup_by_idx(getBlockIdx(R, C, nrb, ncb));
			#pragma oss task taskgroup(tg) \
					in(reps[R-1][C]) in(reps[R+1][C]) \
					in(reps[R][C-1]) in(reps[R][C+1]) \
					inout(reps[R][C]) \
					priority(calculatePriority(R, C, it, maxIt, nrb, ncb)) \
				label("block computation")
			{
				if (ovni_enabled) {
					ovni_mark_push(MARK_TYPE_ROW_PLUS_COL, R+C);
					ovni_mark_push(MARK_TYPE_ROW, R);
					ovni_mark_push(MARK_TYPE_COL, C);
					ovni_mark_push(MARK_TYPE_IT, it+1);
				}

				computeBlock(rows, cols, (R-1)*rbs+1, R*rbs, (C-1)*cbs+1, C*cbs, M);

				if (ovni_enabled) {
					ovni_mark_pop(MARK_TYPE_ROW_PLUS_COL, R+C);
					ovni_mark_pop(MARK_TYPE_ROW, R);
					ovni_mark_pop(MARK_TYPE_COL, C);
					ovni_mark_pop(MARK_TYPE_IT, it+1);
				}
			}
		}
	}
}

double solve(HeatConfiguration *conf, int64_t rows, int64_t cols, int timesteps, void *extraData)
{

	(void) extraData;
	double (*matrix)[cols] = (double (*)[cols]) conf->matrix;
	const int rbs = conf->rbs;
	const int cbs = conf->cbs;

	const int nrb = (rows-2)/rbs+2;
	const int ncb = (cols-2)/cbs+2;
	char representatives[nrb][ncb];
	int ret;

	size_t wsize = nrb * ncb + 100;
	double createStart = getTime();
	fprintf(stderr, "create start: %14f\n", createStart - progStartTime);

	if (submit_window_enabled) {
		ret = nosv_set_submit_window_size(wsize);
		if (ret != NOSV_SUCCESS) {
			fprintf(stderr, "Error setting submit window: \n");
			exit(EXIT_FAILURE);
		}
	}

	for (int t = 0; t < timesteps; ++t) {
		gaussSeidelSolver(rows, cols, rbs, cbs, nrb, ncb, matrix, representatives, t, timesteps);
	}
	double createEnd = getTime();
	fprintf(stderr, "create end: %f\n", createEnd - progStartTime);
	fprintf(stderr, "\tcreate duration: %f\n", createEnd - createStart);

	double execStart = getTime();
	fprintf(stderr, "exec start: %f\n", execStart - progStartTime);

	if (submit_window_enabled) {
		ret = nosv_flush_submit_window();
		if (ret != NOSV_SUCCESS) {
			fprintf(stderr, "Error flushing submit window: \n");
			exit(EXIT_FAILURE);
		}
	}

	#pragma oss taskwait

	double execEnd = getTime();
	fprintf(stderr, "exec end: %f\n", execEnd - progStartTime);
	fprintf(stderr, "\texec duration: %f\n", execEnd - execStart);
	conf->convergenceTimesteps = timesteps;


	return execEnd - execStart;
}

static inline void computeBlock(const int64_t rows, const int64_t cols,
		const int rstart, const int rend,
		const int cstart, const int cend,
		double M[restrict rows][cols])
{

	(void) cend;
	// Assuming square blocks
	const int bs = rend-rstart+1;
	for (int k = 0; k < bs; ++k) {
		#pragma omp simd
		for (int j = 0; j <= k; ++j) {
			const int rr = rstart+k-j;
			const int cc = cstart+j;
			M[rr][cc] = 0.25*(M[rr-1][cc] + M[rr+1][cc] + M[rr][cc-1] + M[rr][cc+1]);
		}
	}
	for (int k = bs-2; k >= 0; --k) {
		#pragma omp simd
		for (int j = 0; j <= k; ++j) {
			const int rr = rstart+bs-j-1;
			const int cc = cstart+bs+j-k-1;
			M[rr][cc] = 0.25*(M[rr-1][cc] + M[rr+1][cc] + M[rr][cc-1] + M[rr][cc+1]);
		}
	}
}

HeatConfiguration conf;
int main(int argc, char **argv)
{
	fprintf(stderr, "Starting Heat Solver with OmpSs-2\n");
	fprintf(stdout, "Starting Heat Solver with OmpSs-2\n");
	progStartTime = getTime();
	check_debug_enabled();
	check_ovni_enabled();
	if (is_debug)
		printf("Debug mode is enabled, PID: %d\n", getpid());
	readConfiguration(argc, argv, &conf);
	refineConfiguration(&conf, conf.rbs, conf.cbs);
	if (conf.verbose)
		printConfiguration(&conf);

	// Parse environment variables with VVV prefix
	char *diags_env = getenv("VVV_DIAGS");
	char *priority_env = getenv("VVV_PRIORITY");
	char *blockfunc_env = getenv("VVV_BLOCKFUNC");
	char *submit_window_env = getenv("VVV_SUBMIT_WINDOW");

	if (!diags_env) {
		fprintf(stderr, "Error: Missing required environment variable VVV_DIAGS\n");
		return 1;
	}

	DIAGS = atoi(diags_env);

	if (priority_env) {
		PRIORITY_MODE = atoi(priority_env);
		if (PRIORITY_MODE < 0 || PRIORITY_MODE > 4) {
			fprintf(stderr, "Error: Invalid priority mode. Must be 0-4.\n");
			return 1;
		}
	}

	if (blockfunc_env) {
		BLOCKFUNC_MODE = atoi(blockfunc_env);
		if (BLOCKFUNC_MODE < 0 || BLOCKFUNC_MODE > 4) {
			fprintf(stderr, "Error: Invalid blockfunc mode. Must be 0-4.\n");
			return 1;
		}
	}

	submit_window_enabled = submit_window_env && strcmp(submit_window_env, "1") == 0 ? 1 : 0;

	// Initialize tglib (reads VVV_TG_ENABLED, VVV_LOWER_LVL, VVV_UPPER_LVL, VVV_TG_POLICY, VVV_AFF_FLEXIBLE)
	tglib_init();
	tglib_create_taskgroups_per_domain();

	// Get number of task groups (domains)
	tglib_tg_array_t tg_array = tglib_get_taskgroup_array();
	ndoms = (int)tg_array.num_tgs;
	if (ndoms == 0) {
		fprintf(stderr, "Warning: No task groups created. Setting ndoms=1 (no affinity).\n");
		ndoms = 1;
	}

	int64_t rows = conf.rows+2;
	int64_t cols = conf.cols+2;

	initialize(&conf, rows, cols, 0);

	#pragma oss taskwait
	fprintf(stderr, "Starting warmup\n");
	if (conf.warmup)
		solve(&conf, rows, cols, 1, NULL);

	if (ovni_enabled)
		ovni_mark_push(MARK_TYPE_IT, 1);
	usleep(500000);
	if (ovni_enabled)
		ovni_mark_pop(MARK_TYPE_IT, 1);

	// Solve the problem
	fprintf(stderr, "Starting benchmark\n");
	double start = getTime();
	double execTime = solve(&conf, rows, cols, conf.timesteps, NULL);
	double end = getTime();
	double delta_time = end - start;
	long niter = conf.convergenceTimesteps;
	long iter_elem = conf.rows * conf.cols;
	long total_elem = iter_elem * niter;
	double throughput = total_elem / delta_time;

	fprintf(stdout, "%14f fake_exec: %14f %14e %14e %8ld %8ld %8d %8d %8ld resultstring\n",
			execTime,
			delta_time, throughput, 0.0,
			conf.rows, conf.cols,
			conf.rbs, conf.cbs, niter);

	if (conf.generateImage)
		writeImage(conf.imageFileName, conf.matrix, rows, cols);

	conf.matrix = NULL;
	finalize(&conf);
	tglib_destroy();

	return 0;
}


void initializeMatrix(const HeatConfiguration *conf, double *matrix, int64_t rows, int64_t cols, int64_t rowOffset)
{
	// Set all elements to zero
	for (int R = 0; R < conf->rows; R += conf->rbs) {
		int rstart = R;
		int rend = MIN(conf->rows, R+conf->rbs);

		for (int C = 0; C < conf->cols; C += conf->cbs) {
			int Rid = R / conf->rbs;
			int Cid = C / conf->cbs;
			int cstart = C;
			int cend = MIN(conf->cols, C+conf->cbs);
			int grp_idx = getBlockIdx(Rid, Cid, conf->rows / conf->rbs + 2, conf->cols / conf->cbs + 2);
			nosv_task_group_t tg = tglib_get_taskgroup_by_idx(grp_idx);

			#pragma oss task taskgroup(tg) \
				label("initialize matrix block")
			{
				// Initialize the block of the matrix
				for (int r = rstart; r < rend; ++r) {
					for (int c = cstart; c < cend; ++c) {
						matrix[r*cols+c] = 0.0;
					}
				}
			}

			(void)cend;
		}
	}

	/* Set the left side to 1.0 */
	for (int i = 0; i < rows; i++) {
		matrix[i*cols] = 1.0;
	}

	(void)(rowOffset);
}

void initialize(HeatConfiguration *conf, int64_t rows, int64_t cols, int64_t rowOffset)
{
	conf->matrix = tglib_mmap_wrapper(rows*cols*sizeof(double));
	if (conf->matrix == NULL) {
		fprintf(stderr, "Error: Memory cannot be allocated!\n");
		exit(1);
	}

	initializeMatrix(conf, conf->matrix, rows, cols, rowOffset);
}
