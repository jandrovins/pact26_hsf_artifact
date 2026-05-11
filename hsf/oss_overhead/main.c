#include "common.h"
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static long ts_to_ns(struct timespec t) {
    return (long)t.tv_sec * 1000000000L + (long)t.tv_nsec;
}

static void save_csv(task_info_t *info, long ntasks, struct timespec wall_start) {
    const char *path = getenv("VVV_OVERHEAD_DATA");
    if (!path) return;

    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(1); }

    fprintf(f, "id,start_ns,end_ns,duration_ns\n");
    long base = ts_to_ns(wall_start);
    for (long i = 0; i < ntasks; i++) {
        long s = ts_to_ns(info[i].start) - base;
        long e = ts_to_ns(info[i].end)   - base;
        fprintf(f, "%d,%ld,%ld,%ld\n", info[i].id, s, e, e - s);
    }
    fclose(f);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <ntasks> <niters>\n", argv[0]);
        return 1;
    }
    long ntasks = atol(argv[1]);
    long niters = atol(argv[2]);

    task_info_t *info   = aligned_alloc(CACHE_LINE, ntasks * sizeof(task_info_t));
    task_info_t *warmup = aligned_alloc(CACHE_LINE, ntasks * sizeof(task_info_t));
    if (!info || !warmup) { perror("aligned_alloc"); return 1; }

    // Warmup pass
    submit_tasks(warmup, ntasks, niters);
    free(warmup);

    // Timed pass
    struct timespec wall_start, wall_end;
    clock_gettime(CLOCK_MONOTONIC, &wall_start);
    submit_tasks(info, ntasks, niters);
    clock_gettime(CLOCK_MONOTONIC, &wall_end);

    double wall_s = (double)(ts_to_ns(wall_end) - ts_to_ns(wall_start)) / 1e9;

    // Convert durations from nanoseconds to microseconds
    double *durations = malloc(ntasks * sizeof(double));
    if (!durations) { perror("malloc"); return 1; }
    for (long i = 0; i < ntasks; i++)
        durations[i] = (double)(ts_to_ns(info[i].end) - ts_to_ns(info[i].start)) / 1e3;

    stats_t st;
    stats_compute(durations, ntasks, &st);

    printf("wall_s=%.6f  ntasks=%ld  niters=%ld\n", wall_s, ntasks, niters);
    stats_print(&st);

	//save_csv(info, ntasks, wall_start);

    free(durations);
    free(info);
    return 0;
}
