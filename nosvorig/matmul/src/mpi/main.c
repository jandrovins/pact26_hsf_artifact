#include "common/matmul.h"

#include <mpi.h>
#ifdef TAMPI
#include <TAMPI.h>
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int rank = 0, nranks;

int main(int argc, char **argv)
{
	matmul_init();

	// Initialize MPI and TAMPI
	int required = MPI_THREAD_MULTIPLE;
	int provided;
	MPI_Init_thread(&argc, &argv, required, &provided);
	if (provided != required) {
		fprintf(stderr, "error: %d not supported", required);
		exit(1);
	}

	MPI_Barrier(MPI_COMM_WORLD);

	MPI_Info info;
	MPI_Info_create(&info);
	MPI_Info_set(info, "mpi_assert_no_any_tag", "true");
	MPI_Info_set(info, "mpi_assert_no_any_source", "true");
	MPI_Info_set(info, "mpi_assert_exact_length", "true");
	MPI_Comm_set_info(MPI_COMM_WORLD, info);
	MPI_Info_free(&info);

	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &nranks);

	matmul_conf_t conf;
	if (!rank)
		matmul_getconf(argc, argv, &conf, nranks);

	MPI_Bcast(&conf, sizeof(conf), MPI_BYTE, 0, MPI_COMM_WORLD);

	const size_t n = conf.n;
	const size_t m_per_rank = conf.m / nranks;
	const size_t ts = conf.ts;

	if (conf.n % nranks)
		matmul_fail("The matrix size must be divisible by the number of processes");
	if (conf.n % conf.ts != 0 || m_per_rank % conf.ts != 0)
		matmul_fail("The matrix size must be divisible by the tile size");

	matmul_t matmul;
	matmul_setup(&conf, &matmul, m_per_rank);

	MPI_Barrier(MPI_COMM_WORLD);

	if (conf.warmup) {
		matmul_solve(n, m_per_rank, ts, &matmul, conf.warmup);

		MPI_Barrier(MPI_COMM_WORLD);
	}

	// Execute matmul solver
	double start = matmul_gettime();

	matmul_solve(n, m_per_rank, ts, &matmul, conf.timesteps);

	MPI_Barrier(MPI_COMM_WORLD);

	double end = matmul_gettime();

	double expected = (double)(conf.timesteps + conf.warmup) * matmul.alpha * matmul.beta * n;

	// Check results
	int errors = matmul_check(conf.m, m_per_rank, ts, matmul.C, expected);

	MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

	if (!rank && errors > 0)
		fprintf(stderr, "Results checked: WRONG, mismatches: %d\n", errors);
	else if (!rank) {
		double t = end - start;
		matmul_report(t, &conf);
	}

	// Finalize MPI and TAMPI
	MPI_Finalize();

	matmul_finish();

	return 0;
}
