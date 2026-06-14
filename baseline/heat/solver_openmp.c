#include "heat.h"

const char *
summary(void)
{
	return "Parallel version using OpenMP tasks";
}

static inline void gaussSeidelSolver(int64_t rows, int64_t cols, int rbs, int cbs, int nrb, int ncb, double M[rows][cols], char reps[nrb][ncb])
{
	for (int R = 1; R < nrb-1; ++R) {
		for (int C = 1; C < ncb-1; ++C) {
#pragma omp task depend(in:reps[R-1][C], reps[R+1][C], reps[R][C-1], reps[R][C+1]) depend(inout:reps[R][C])
			computeBlock(rows, cols, (R-1)*rbs+1, R*rbs, (C-1)*cbs+1, C*cbs, M);
		}
	}
}

void solve(HeatConfiguration *conf, int64_t rows, int64_t cols, int timesteps, void *extraData)
{
	(void) extraData;
	double (*matrix)[cols] = (double (*)[cols]) conf->matrix;
	const int rbs = conf->rbs;
	const int cbs = conf->cbs;

	const int nrb = (rows-2)/rbs+2;
	const int ncb = (cols-2)/cbs+2;
	char representatives[nrb][ncb];

	#pragma omp parallel
	#pragma omp single
	for (int t = 0; t < timesteps; ++t) {
		gaussSeidelSolver(rows, cols, rbs, cbs, nrb, ncb, matrix, representatives);
	}

	conf->convergenceTimesteps = timesteps;
}
