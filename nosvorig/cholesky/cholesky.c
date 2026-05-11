#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <numaif.h>

#ifdef USE_MKL
#include <mkl.h>
#else
#include <cblas.h>
#include <lapacke.h>
#endif

#include "valloc.h"

#define PRIME1 293
#define PRIME2 719

long nblocks_per_dim = -1;

static void cholesky(long N, long TS, double (*A)[N/TS][TS][TS])
{
	const int use_priority = 1;
        int prio_syrk = use_priority ? 10000 : 0;
        int prio_potrf = use_priority ? 20000 : 0;
        int prio_trsm = use_priority ? 30000 : 0;
        // IDEA: tener un taskgroup for iteration k, and all tasks in that iteration use that taskgroup. Set taskgruop priority to N/TS - k

	for (long k = 0; k < N/TS; k++) {
                int potrf_prio = use_priority ? ((nblocks_per_dim - k) + prio_potrf) : 0; // Ensure potrf has higher priority than any trsm or syrk in the same iteration, regardless of i and
		#pragma oss task priority(potrf_prio) inout(A[k][k]) label("potrf")
		LAPACKE_dpotrf(LAPACK_ROW_MAJOR, 'L', TS, (double *) A[k][k], TS);
		
		for (long i = k+1; i < N/TS; i++) {
			int prio = use_priority ? (nblocks_per_dim - i + prio_trsm) : 0;
			#pragma oss task priority(prio) in(A[k][k]) inout(A[i][k]) label("trsm")
			cblas_dtrsm(CblasRowMajor, CblasRight, CblasLower, CblasTrans, CblasNonUnit, TS, TS, 1.0, (double const *) A[k][k], TS, (double *) A[i][k], TS);
		}
		
		for (long i = k+1; i < N/TS; i++) {
			for (long j = k+1; j < i; j++) {
				#pragma oss task in(A[i][k], A[j][k]) inout(A[i][j]) label("gemm")
				cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasTrans, TS, TS, TS, -1.0, (double const *) A[i][k], TS, (double const *) A[j][k], TS, 1.0, (double *) A[i][j], TS);
			}
                        int prio = use_priority ? (nblocks_per_dim - i + prio_syrk) : 0;
			#pragma oss task priority(prio) in(A[i][k]) inout(A[i][i]) label("syrk")
			cblas_dsyrk(CblasRowMajor, CblasLower, CblasNoTrans, TS, TS, -1.0, (double const *) A[i][k], TS, 1.0, (double *) A[i][i], TS);
		}
	}
}

static void initialize(long N, long TS, double (*a)[N/TS][TS][TS])
{
//	for (long i=0; i < N; i++) {
//		for (long j=0; j < N; j++) {
//			// Generate a value that makes the matrix symmetric positive definite
//			double value = (((i+j) % PRIME1) + 1) * (((i+j) % PRIME2) + 1);
//			if (i == j) {
//				value += PRIME1 * PRIME2;
//			}
//
//			// Tiled layout
//			a[i/TS][j/TS][i%TS][j%TS] = value;
//		}
//	}
	for (long i = 0; i < N; i += TS) {
		for (long j = 0; j < N; j += TS) {
			#pragma oss task label("init_tile")
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
	#pragma oss taskwait

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

	nblocks_per_dim = (n + ts - 1) / ts;

	typedef double (*matrix_t)[n/ts][ts][ts];
	valloc_init();

	//matrix_t a = (matrix_t) malloc(sizeof(double) * n * n);
	matrix_t a = (matrix_t)valloc(sizeof(double) * n * n);
	printf("Matrix size: %ld bytes\n", sizeof(double) * n * n);
	printf("Matrix dimensions: %ld x %ld\n", n, n);

	// Warmup iteration
	initialize(n, ts, a);
	cholesky(n, ts, a);

	// Real initialization
	initialize(n, ts, a);

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (int i = 0; i < iterations; i++)
		cholesky(n, ts, a);

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


	valloc_cleanup();

	return 0;
}
