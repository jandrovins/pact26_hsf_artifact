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

static void multisaxpy(double *x, double *y, double alpha, long N, long its)
{
	(void) alpha;

	#pragma omp parallel
	for (long iteration=0; iteration < its; iteration++) {
		#pragma omp for schedule(static)
		for (long i=0; i < N; i++) {
			y[i] += alpha * x[i];
		}
	}
}


static void initialize(double *data, double value, long N)
{
#pragma omp parallel for schedule(static)
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
	long its = 1L;

	if (argc > 4 || (argc >= 2 && strcmp(argv[1], "-h") == 0)) {
		std::cerr << "[USAGE] " << argv[0] << " [-h] elements iterations" << std::endl;
		return 1;
	}

	if (argc >= 2)
		n = atol(argv[1]);
	if (argc >= 3)
		its = atol(argv[2]);

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

	double *x = use_numa
		? (double *) numa_alloc_interleaved_subset(n * sizeof(double), node_mask)
		: (double *) malloc(n * sizeof(double));
	double *y = use_numa
		? (double *) numa_alloc_interleaved_subset(n * sizeof(double), node_mask)
		: (double *) malloc(n * sizeof(double));

	if (node_mask)
		numa_free_nodemask(node_mask);

	initialize(x, 1.0, n);
	initialize(y, 0.0, n);
	print_numa_distribution("x", x, n * sizeof(double));
	print_numa_distribution("y", y, n * sizeof(double));

	// Warmup iteration
	multisaxpy(x, y, 1.0, n, its);

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);

	multisaxpy(x, y, 1.0, n, its);

	clock_gettime(CLOCK_MONOTONIC, &end);

	double duration = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
	duration /= 1000000;

	double performance = n;
	performance *= its;
	performance = performance / duration;
	performance /= 1000000000;

	printf("%14e %14e %14ld %14s %14ld %s\n", duration, performance, n, "NaN", its, BENCH6_NAME);

	return 0;
}
