#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <numaif.h>
#include <numa.h>

#ifdef USE_MKL
#include <mkl.h>
#else
#include <blis.h>
#include <cblas.h>
#include <lapacke.h>

/* AOCL-LAPACK progress callback support */
#ifndef integer
typedef int integer;
#endif

extern void aocl_fla_set_progress(int (*fn)(const char* const, const integer,
    const integer* const, const integer* const, const integer* const));

static int aocl_fla_progress(const char* const api, const integer lenapi,
    const integer* const progress, const integer* const current_thread,
    const integer* const total_threads)
{
    (void)lenapi;
    printf("[AOCL progress] api=%s thread=%d progress=%d total_threads=%d\n",
        api, *current_thread, *progress, *total_threads);
    return 0;
}
#endif

#include "valloc.h"

#define PRIME1 293
#define PRIME2 719

static void initialize(long N, long TS, double *a)
{
	for (long i = 0; i < N; i += TS) {
		for (long j = 0; j < N; j += TS) {
			for (long ii = i; ii < i + TS; ii++) {
				for (long jj = j; jj < j + TS; jj++) {
					// Generate a value that makes the matrix symmetric positive definite
					double value = (((ii+jj) % PRIME1) + 1) * (((ii+jj) % PRIME2) + 1);
					if (ii == jj) {
						value += PRIME1 * PRIME2;
					}

					// Row-major layout
					a[ii * N + jj] = value;
				}
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
}

int main(int argc, char **argv)
{
	long n = 4L * 1024L;
	long ts = 1024L;

	if (argc > 4 || (argc >= 2 && strcmp(argv[1], "-h") == 0)) {
		fprintf(stderr, "[USAGE] %s N tasksize\n", argv[0]);
		fprintf(stderr, "  tasksize must divide N\n");
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

	valloc_init();

#ifndef USE_MKL
	printf("AOCL-BLAS version       : %s\n",  bli_info_get_version_str());
	printf("AOCL-BLAS OpenMP        : %s\n",  bli_info_get_enable_openmp()    ? "enabled" : "disabled");
	printf("AOCL-BLAS pthreads      : %s\n",  bli_info_get_enable_pthreads()  ? "enabled" : "disabled");
	printf("AOCL-BLAS threading     : %s\n",  bli_info_get_enable_threading() ? "enabled" : "disabled");
	printf("AOCL-BLAS num threads   : %ld\n", (long)bli_thread_get_num_threads());
#endif

	size_t mat_bytes = sizeof(double) * n * n;
	int use_interleaved = 0;
	{
		const char *env = getenv("VVV_NUMA_INTERLEAVED");
		if (env && atoi(env) == 1) use_interleaved = 1;
	}
	printf("NUMA interleaved allocation: %s\n", use_interleaved ? "yes" : "no");

	double *a;
	if (use_interleaved) {
		// Derive the set of NUMA nodes reachable from this process's CPU affinity
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		if (sched_getaffinity(0, sizeof(cpuset), &cpuset) != 0) {
			perror("sched_getaffinity failed");
			return 1;
		}

		int max_cpus = numa_num_configured_cpus();
		struct bitmask *nodemask = numa_bitmask_alloc(numa_num_configured_nodes());
		numa_bitmask_clearall(nodemask);

		for (int cpu = 0; cpu < max_cpus; cpu++) {
			if (CPU_ISSET(cpu, &cpuset)) {
				int node = numa_node_of_cpu(cpu);
				if (node >= 0)
					numa_bitmask_setbit(nodemask, node);
			}
		}

		// Print which nodes will be used
		printf("NUMA interleaved subset nodes:");
		for (unsigned int nd = 0; nd < nodemask->size; nd++) {
			if (numa_bitmask_isbitset(nodemask, nd))
				printf(" %u", nd);
		}
		printf("\n");

		a = (double *)numa_alloc_interleaved_subset(mat_bytes, nodemask);
		numa_bitmask_free(nodemask);
		if (!a) { fprintf(stderr, "numa_alloc_interleaved_subset failed\n"); return 1; }
	} else {
		a = (double *)valloc(mat_bytes);
	}
	printf("Matrix size: %zu bytes\n", mat_bytes);
	printf("Matrix dimensions: %ld x %ld\n", n, n);

	// Real initialization
	initialize(n, ts, a);

	//printf("PID: %d\n", getpid());
	//fflush(stdout);
	//volatile int x = 0;
	//while (!x) {
	//	usleep(100);
	//}

	struct timespec start, end;

#ifndef USE_MKL
	//aocl_fla_set_progress(aocl_fla_progress);
#endif

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (int i = 0; i < iterations; i++)
		LAPACKE_dpotrf(LAPACK_ROW_MAJOR, 'L', n, a, n);

	clock_gettime(CLOCK_MONOTONIC, &end);

	double duration = (end.tv_sec - start.tv_sec) * 1000000 + ((double)(end.tv_nsec - start.tv_nsec)) / 1000.0;
	duration /= 1000000;

	// GFlops
	double performance = (0.33*n*n*n + 0.5*n*n + 0.17*n) * iterations;
	performance = performance / duration;
	performance /= 1000000000;

	printf("Printing result: ");
	printf("%14e %14e %14ld\n", duration, performance, n);

	if (use_interleaved) {
		numa_free(a, mat_bytes);
	} else {
		valloc_cleanup();
	}

	return 0;
}
