#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heat.h"
#include "valloc.h"

static int priority_enabled = 0;

/* Forward declaration — defined below main() */
void computeBlock(const int64_t rows, const int64_t cols,
		const int rstart, const int rend,
		const int cstart, const int cend,
		double M[restrict rows][cols]);

const char *
summary(void)
{
	return "Parallel version using OmpSs-2 tasks (priority baseline, no tglib)";
}

static inline int calculatePriority(int it, int maxIt)
{
	return priority_enabled ? (maxIt - it) : 0;
}

static inline void gaussSeidelSolver(int64_t rows, int64_t cols, int rbs, int cbs, int nrb, int ncb, double M[rows][cols], char reps[nrb][ncb], int it, int maxIt)
{
	for (int R = 1; R < nrb-1; ++R) {
		for (int C = 1; C < ncb-1; ++C) {
			int prio = calculatePriority(it, maxIt);
			#pragma oss task \
					in(reps[R-1][C]) in(reps[R+1][C]) \
					in(reps[R][C-1]) in(reps[R][C+1]) \
					inout(reps[R][C]) \
					priority(prio) \
				label("block computation")
			{
				computeBlock(rows, cols, (R-1)*rbs+1, R*rbs,
				             (C-1)*cbs+1, C*cbs, M);
			}
		}
	}
}

double solve(HeatConfiguration *conf, int64_t rows, int64_t cols, int timesteps, void *extraData)
{
	(void) extraData;
	double (*matrix)[cols] = (double (*)[cols]) conf->matrix;
	const int rbs = conf->rbs, cbs = conf->cbs;
	const int nrb = (rows-2)/rbs+2;
	const int ncb = (cols-2)/cbs+2;
	char representatives[nrb][ncb];

	for (int t = 0; t < timesteps; ++t)
		gaussSeidelSolver(rows, cols, rbs, cbs, nrb, ncb, matrix, representatives, t, timesteps);

	#pragma oss taskwait
	conf->convergenceTimesteps = timesteps;
	return 0.0;   /* no residual computed */
}

void computeBlock(const int64_t rows, const int64_t cols,
		const int rstart, const int rend,
		const int cstart, const int cend,
		double M[restrict rows][cols])
{
	(void) cend;
	/* Assuming square blocks */
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
	valloc_init();
	readConfiguration(argc, argv, &conf);
	refineConfiguration(&conf, conf.rbs, conf.cbs);
	if (conf.verbose) printConfiguration(&conf);

	char *priority_env = getenv("VVV_PRIORITY");
	if (priority_env) priority_enabled = atoi(priority_env) != 0;

	int64_t rows = conf.rows + 2;
	int64_t cols = conf.cols + 2;

	initialize(&conf, rows, cols, 0);
	#pragma oss taskwait

	printNumaDistribution("matrix", conf.matrix, (size_t)rows * cols * sizeof(double));

	if (conf.warmup) solve(&conf, rows, cols, 1, NULL);

	double start = getTime();
	solve(&conf, rows, cols, conf.timesteps, NULL);
	double end = getTime();
	double delta_time = end - start;
	long niter = conf.convergenceTimesteps;
	double throughput = (double)(conf.rows * conf.cols) * niter / delta_time;

	fprintf(stdout, "%14e %14e %14e %8ld %8ld %8d %8d %8ld %s\n",
	        delta_time, throughput, 0.0,
	        conf.rows, conf.cols, conf.rbs, conf.cbs, niter, "heat_result");

	if (conf.generateImage)
		writeImage(conf.imageFileName, conf.matrix, rows, cols);

	finalize(&conf);
	valloc_cleanup();
	return 0;
}
