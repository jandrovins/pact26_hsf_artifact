#include <cassert>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <algorithm>

#include <ovni.h>
#include <nosv.h>
#include <nosv/affinity.h>
#include <nosv/hwinfo.h>
#include <tglib.h>

#include <sys/mman.h>

#define __maybe_unused __attribute__((unused))
#define vvvassert(expr) \
	do { \
		if (!(expr)) { \
			std::cerr << "Assertion failed: " << #expr << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
			abort(); \
		} \
	} while (0)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"

constexpr static std::size_t huge_page_size = 1 << 21; // 2 MiB

enum {
	MARK_TYPE_BLOCKID = 21,
	MARK_TYPE_IT = 22,
};

struct timespec prog_start, prog_end;
struct timespec start, stop;

// Debug/ovni flags
static bool is_debug = false;
static bool ovni_enabled = false;

static void check_debug_enabled() {
	const char* debug_env = getenv("VVV_DEBUG");
	is_debug = debug_env && strcmp(debug_env, "1") == 0;
}

static void check_ovni_enabled() {
	const char* vvv_ovni_env = getenv("VVV_OVNI");
	ovni_enabled = vvv_ovni_env && strcmp(vvv_ovni_env, "1") == 0;
	if (is_debug)
		std::cerr << "OVNI enabled: " << (ovni_enabled ? "true" : "false") << std::endl;
}

static bool is_debug_enabled() {
	return is_debug;
}

class MultiSAXPY {
private:
	long N;
	long TS;
	long its;
	double *x;
	double *y;
	long num_blocks;

public:
	MultiSAXPY(long _n, long _ts, long _its)
		: N(_n), TS(_ts), its(_its)
	{
		if (N % TS != 0) {
			std::cerr << "Error: N (" << N << ") must be a multiple of TS (" << TS << ")" << std::endl;
			exit(EXIT_FAILURE);
		}

		num_blocks = N / TS;

		// Initialize tglib (reads VVV_* env vars)
		tglib_init();
		tglib_create_taskgroups_per_domain();

		// Allocate memory with huge pages
		int ret;
		ret = posix_memalign((void**)&x, huge_page_size, N * sizeof(double));
		madvise(x, N * sizeof(double), MADV_HUGEPAGE);
		if (ret != 0) {
			std::cerr << "Error allocating aligned memory for x: " << ret << std::endl;
			exit(EXIT_FAILURE);
		}
		ret = posix_memalign((void**)&y, huge_page_size, N * sizeof(double));
		madvise(y, N * sizeof(double), MADV_HUGEPAGE);
		if (ret != 0) {
			std::cerr << "Error allocating aligned memory for y: " << ret << std::endl;
			exit(EXIT_FAILURE);
		}

		// First-touch initialization with taskgroups for NUMA placement
		initialize(x, 1.0);
		initialize(y, 0.0);
		#pragma oss taskwait
	}

	~MultiSAXPY() {
		free(x);
		free(y);
		tglib_destroy();
	}

	void axpy_task(double * __restrict__ x_ptr, double * __restrict__ y_ptr, double alpha, __maybe_unused int blockid, __maybe_unused int it)
	{
		int bid_mark = blockid + 1;
		int it_mark = it + 1;
		if (ovni_enabled) {
			ovni_mark_push(MARK_TYPE_BLOCKID, bid_mark);
			ovni_mark_push(MARK_TYPE_IT, it_mark);
		}
		for (long i = 0; i < TS; i++) {
			y_ptr[i] += alpha * x_ptr[i];
		}
		if (ovni_enabled) {
			ovni_mark_pop(MARK_TYPE_BLOCKID, bid_mark);
			ovni_mark_pop(MARK_TYPE_IT, it_mark);
		}
	}

	void axpy(double alpha, int it)
	{
		for (long i = 0; i < N; i += TS) {
			int blockid = (int)(i / TS);
			nosv_task_group_t tg = tglib_get_taskgroup_by_idx(blockid);
			int prio = (int)(num_blocks - blockid);
			#pragma oss task taskgroup(tg) priority(prio) in(x[i]) inout(y[i]) label("axpy_task")
			axpy_task(x + i, y + i, alpha, blockid, it);
		}
	}

	double multisaxpy(double alpha, long its_run)
	{
		struct timespec run_start, run_end;
		clock_gettime(CLOCK_MONOTONIC, &run_start);

		for (long iteration = 0; iteration < its_run; iteration++) {
			axpy(alpha, (int)iteration);
		}
		#pragma oss taskwait

		clock_gettime(CLOCK_MONOTONIC, &run_end);
		double task_duration =
			(run_end.tv_sec - run_start.tv_sec)
			+ (run_end.tv_nsec - run_start.tv_nsec) / 1e9;

		if (is_debug_enabled()) {
			std::cerr << "Task duration: " << task_duration << " seconds" << std::endl;
		}

		return task_duration;
	}

	void initialize(double *data, double value)
	{
		for (long i = 0; i < N; i += TS) {
			int blockid = (int)(i / TS);
			nosv_task_group_t tg = tglib_get_taskgroup_by_idx(blockid);
			#pragma oss task taskgroup(tg) inout(data[i]) label("initialize_task")
			for (long j = i; j < i + TS; j++) {
				if (j >= N) break;
				data[j] = value;
			}
		}
	}
};

int main(int argc, char **argv)
{
	check_debug_enabled();
	check_ovni_enabled();
	if (is_debug_enabled())
		std::cerr << "Debug mode is enabled, PID: " << getpid() << std::endl;

	clock_gettime(CLOCK_MONOTONIC, &prog_start);

	if (argc != 4 || (strcmp(argv[1], "-h") == 0)) {
		std::cerr << "Usage: " << argv[0] << " <elements> <chunksize> <iterations>" << std::endl;
		std::cerr << std::endl;
		std::cerr << "Task group configuration via environment variables:" << std::endl;
		std::cerr << "  VVV_TG_ENABLED  : 0 or 1 (default: 1)" << std::endl;
		std::cerr << "  VVV_LOWER_LVL   : node, numa, cs, core, cpu (default: cs)" << std::endl;
		std::cerr << "  VVV_UPPER_LVL   : node, numa, cs, core, cpu (default: numa)" << std::endl;
		std::cerr << "  VVV_AFF_FLEXIBLE: 0 or 1 (default: 0)" << std::endl;
		std::cerr << "  VVV_TG_POLICY   : PRIO or FIFO (default: PRIO)" << std::endl;
		return 1;
	}

	long n   = atol(argv[1]);
	long ts  = atol(argv[2]);
	long its = atol(argv[3]);

	if (n % ts != 0) {
		std::cerr << "Error: n (" << n << ") must be a multiple of ts (" << ts << ")" << std::endl;
		return 1;
	}

	if (ovni_enabled) {
		ovni_mark_type(MARK_TYPE_BLOCKID, OVNI_MARK_STACK, "MARKS FOR BLOCK ID");
		ovni_mark_type(MARK_TYPE_IT, OVNI_MARK_STACK, "MARKS FOR ITERATION");
	}

	MultiSAXPY msaxpy(n, ts, its);

	// Warmup
	long warmup_its = 10 < its ? 10 : its;
	msaxpy.multisaxpy(1.2, warmup_its);
	usleep(500000);
	#pragma oss taskwait

	// Timed run
	clock_gettime(CLOCK_MONOTONIC, &start);
	double duration = msaxpy.multisaxpy(1.0, its);
	clock_gettime(CLOCK_MONOTONIC, &stop);

	// Compute performance
	double flops = 2.0 * (double)n * (double)its; // axpy: 1 mul + 1 add per element
	double gflops = flops / duration / 1e9;

	// Output in parseable format
	printf("Printing result %14e %ld %ld %ld %14e\n", duration, n, ts, its, gflops);

	clock_gettime(CLOCK_MONOTONIC, &prog_end);
	double prog_duration = (prog_end.tv_sec - prog_start.tv_sec)
		+ (prog_end.tv_nsec - prog_start.tv_nsec) / 1e9;
	if (is_debug_enabled())
		fprintf(stderr, "Total program duration: %14e seconds\n", prog_duration);

	return 0;
}
