#include <cassert>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <algorithm>
#include <numa.h>
#include <sched.h>
#include <numaif.h>
#include <vector>
#include <map>

#include <mpi.h>

static void axpy_task(double *x, double *y, double alpha, long N)
{
	for (long i=0; i < N; ++i) {
		y[i] += alpha * x[i];
	}
}

static void axpy(double *x, double *y, double alpha, long N, long TS)
{
	#pragma omp parallel for schedule(static)
	for (long i=0; i < N; i+=TS) {
		axpy_task(x+i, y+i, alpha, std::min(TS, N-i));
	}
}

static void multisaxpy(double *x, double *y, double alpha, long N, long TS, long its)
{
	(void) alpha;

	for (long iteration=0; iteration < its; iteration++) {
		axpy(x, y, 1.0, N, TS);
	}
}


static void initialize(double *data, double value, long N, long)
{
	for (long i=0; i < N; i++) {
		data[i] = value;
	}
}

static void print_numa_distribution(const char *name, void *ptr, size_t bytes)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t npages = (bytes + page_size - 1) / page_size;

	std::vector<void *> pages(npages);
	std::vector<int>    status(npages);
	for (size_t i = 0; i < npages; i++)
		pages[i] = (char *)ptr + i * page_size;

	move_pages(0, npages, pages.data(), nullptr, status.data(), 0);

	std::map<int, size_t> counts;
	for (size_t i = 0; i < npages; i++)
		counts[status[i]]++;

	fprintf(stderr, "NUMA distribution for %s (%zu pages):\n", name, npages);
	for (auto &[node, cnt] : counts)
		fprintf(stderr, "  node %2d: %6zu pages  (%.1f%%)\n",
			node, cnt, 100.0 * cnt / npages);
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

	int provided;
	MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
	assert(provided == MPI_THREAD_MULTIPLE);

	int rank, rank_size;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &rank_size);
	assert(rank_size > 0);

	if (argc >= 2)
		n = atol(argv[1]);
	if (argc >= 3)
		ts = atol(argv[2]);
	if (argc >= 4)
		its = atol(argv[3]);

	if (n % rank_size != 0) {
		std::cerr << "elements must be divisable by the number of MPI processes" << std::endl;
		return 1;
	}

	const bool use_numa = (numa_available() >= 0) && [] {
		const char *v = getenv("VVV_NUMA_INTERLEAVED");
		return v && v[0] == '1';
	}();

	struct bitmask *node_mask = nullptr;
	if (use_numa) {
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);

		node_mask = numa_allocate_nodemask();
		int ncpus = numa_num_configured_cpus();
		for (int cpu = 0; cpu < ncpus; cpu++) {
			if (CPU_ISSET(cpu, &cpuset)) {
				int node = numa_node_of_cpu(cpu);
				if (node >= 0)
					numa_bitmask_setbit(node_mask, node);
			}
		}
	}

	double *x = nullptr, *y = nullptr;
	if (rank == 0) {
		x = use_numa
			? (double *) numa_alloc_interleaved_subset(n * sizeof(double), node_mask)
			: (double *) malloc(n * sizeof(double));
		y = use_numa
			? (double *) numa_alloc_interleaved_subset(n * sizeof(double), node_mask)
			: (double *) malloc(n * sizeof(double));
		initialize(x, 1.0, n, ts);
		initialize(y, 0.0, n, ts);
	}

	MPI_Barrier(MPI_COMM_WORLD);

	unsigned long long int local_size = n / rank_size;
	double *local_x = use_numa
		? (double *) numa_alloc_interleaved_subset(local_size * sizeof(double), node_mask)
		: (double *) malloc(local_size * sizeof(double));
	double *local_y = use_numa
		? (double *) numa_alloc_interleaved_subset(local_size * sizeof(double), node_mask)
		: (double *) malloc(local_size * sizeof(double));

	if (node_mask)
		numa_free_nodemask(node_mask);

	MPI_Scatter(x, local_size, MPI_DOUBLE, local_x, local_size, MPI_DOUBLE, 0, MPI_COMM_WORLD);
	MPI_Scatter(y, local_size, MPI_DOUBLE, local_y, local_size, MPI_DOUBLE, 0, MPI_COMM_WORLD);
	print_numa_distribution("local_x", local_x, local_size * sizeof(double));
	print_numa_distribution("local_y", local_y, local_size * sizeof(double));

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);

	multisaxpy(local_x, local_y, 1.0, local_size, ts, its);

	clock_gettime(CLOCK_MONOTONIC, &end);

	MPI_Barrier(MPI_COMM_WORLD);
	MPI_Gather(local_y, local_size, MPI_DOUBLE, y, local_size, MPI_DOUBLE, 0, MPI_COMM_WORLD);

	if (rank == 0) {
		double duration = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
		duration /= 1000000;

		double performance = 2.0 * n; // saxpy: 1 mul + 1 add per element
		performance *= its;
		performance = performance / duration;
		performance /= 1000000000;

		printf("%14e %14e %14ld %14ld %14ld %s\n", duration, performance, n, ts, its, BENCH6_NAME);
	}

	MPI_Finalize();
	return 0;
}
