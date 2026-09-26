#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <algorithm>
#include <stdlib.h>
#include <valloc.h>

static void axpy_task(double *x, double *y, double alpha, long N)
{
	for (long i=0; i < N; ++i) {
		y[i] += alpha * x[i];
	}
}

static void axpy(double *x, double *y, double alpha, long N, long TS,
                 long num_blocks, int prio_enabled)
{
	for (long i = 0; i < N; i += TS) {
		long blockid = i / TS;
		int prio = prio_enabled ? (int)(num_blocks - blockid) : 0;
		#pragma oss task label("axpy_task") priority(prio) in(x[i:TS]) inout(y[i:TS])
		axpy_task(x+i, y+i, alpha, std::min(TS, N-i));
	}
}

static void multisaxpy(double *x, double *y, double alpha, long N, long TS,
                        long its, int prio_enabled)
{
	(void) alpha;
	long num_blocks = (N + TS - 1) / TS;
	for (long iteration = 0; iteration < its; iteration++) {
		axpy(x, y, 1.0, N, TS, num_blocks, prio_enabled);
	}
	#pragma oss taskwait
}

static void initialize(double *data, double value, long N, long TS)
{
	for (long i = 0; i < N; i += TS) {
		#pragma oss task inout(data[i]) label("initialize_task")
		for (long j = i; j < i + TS; j++) {
			if (j >= N) break;
			data[j] = value;
		}
	}
#pragma oss taskwait
}

int main(int argc, char **argv)
{
	long n   = 2000000L;
	long ts  = 50000L;
	long its = 500L;

	if (argc > 4 || (argc >= 2 && strcmp(argv[1], "-h") == 0)) {
		std::cerr << "[USAGE] " << argv[0] << " [-h] elements chunksize iterations" << std::endl;
		return 1;
	}

	if (argc >= 2)
		n = atol(argv[1]);
	if (argc >= 3)
		ts = atol(argv[2]);
	if (argc >= 4)
		its = atol(argv[3]);

	valloc_init();

	int prio_enabled = 0;
	const char *prio_env = getenv("VVV_PRIO_ENABLED");
	if (prio_env && prio_env[0] == '1')
		prio_enabled = 1;

	double *x = (double *) valloc(n * sizeof(double));
	double *y = (double *) valloc(n * sizeof(double));

	initialize(x, 1.0, n, ts);
	initialize(y, 0.0, n, ts);

	// Warmup iteration
	multisaxpy(x, y, 1.0, n, ts, its, prio_enabled);

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);

	multisaxpy(x, y, 1.0, n, ts, its, prio_enabled);

	clock_gettime(CLOCK_MONOTONIC, &end);

	double duration = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
	duration /= 1000000;

	double performance = 2.0 * n; // saxpy: 1 mul + 1 add per element
	performance *= its;
	performance = performance / duration;
	performance /= 1000000000;

	printf("%14e %14e %14ld %14ld %14ld %s\n", duration, performance, n, ts, its, "result_multisaxpy");

	valloc_cleanup();
	return 0;
}
