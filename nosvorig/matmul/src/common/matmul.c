#include "matmul.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include <nosv/hwinfo.h>
#include "valloc.h"

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

	typedef double (*matrix_MxN_t)[n/ts][ts][ts];
	typedef double (*matrix_NxM_t)[m_per_rank/ts][ts][ts];

	matrix_MxN_t A, remote1, remote2;
	matrix_NxM_t B, C;

	size_t size_A = m_per_rank * n * sizeof(double);
	size_t size_B = n * m_per_rank * sizeof(double);
	size_t size_C = m * m_per_rank * sizeof(double);

	fprintf(stderr, "Allocating %.2f GiB of memory\n", (double)(size_A * 3 + size_B + size_C) / (1024.0 * 1024.0 * 1024.0));

	valloc_init();
	A = (matrix_MxN_t) valloc(size_A);
	B = (matrix_NxM_t) valloc(size_B);
	C = (matrix_NxM_t) valloc(size_C);
	remote1 = (matrix_MxN_t) valloc(size_A);
	remote2 = (matrix_MxN_t) valloc(size_A);
	//A = malloc(size_A);
	//B = malloc(size_B);
	//C = malloc(size_C);
	//remote1 = malloc(size_A);
	//remote2 = malloc(size_A);

	if (!A || !B || !C || !remote1 || !remote2)
		matmul_fail("not enough memory");

	char *env = getenv("VVV_NUMA_INTERLEAVED");
	int hpccg_numa_interleaved = env && atoi(env) != 0;
	if (hpccg_numa_interleaved) {
		// Initialize matrices
		for (size_t i = 0; i < m_per_rank/ts; ++i) {
			for (size_t k = 0; k < n/ts; ++k) {
				#pragma oss task out(A[i][k]) label("matmul_fill_tile A")
				matmul_fill_tile(ts, A[i][k], 1.0);
			}
		}
		for (size_t k = 0; k < n/ts; ++k) {
			for (size_t j = 0; j < m_per_rank/ts; ++j) {
				#pragma oss task out(B[k][j]) label("matmul_fill_tile B")
				matmul_fill_tile(ts, B[k][j], 1.0);
			}
		}
		for (size_t i = 0; i < m/ts; ++i) {
			for (size_t j = 0; j < m_per_rank/ts; ++j) {
				#pragma oss task out(C[i][j]) label("matmul_fill_tile C")
				matmul_fill_tile(ts, C[i][j], 0.0);
			}
		}
		#pragma oss taskwait
	} else {
		// Initialize matrices
		for (size_t i = 0; i < m_per_rank/ts; ++i) {
			for (size_t k = 0; k < n/ts; ++k) {
				matmul_fill_tile(ts, A[i][k], 1.0);
			}
		}
		for (size_t k = 0; k < n/ts; ++k) {
			for (size_t j = 0; j < m_per_rank/ts; ++j) {
				matmul_fill_tile(ts, B[k][j], 1.0);
			}
		}
		for (size_t i = 0; i < m/ts; ++i) {
			for (size_t j = 0; j < m_per_rank/ts; ++j) {
				matmul_fill_tile(ts, C[i][j], 0.0);
			}
		}
	}

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
