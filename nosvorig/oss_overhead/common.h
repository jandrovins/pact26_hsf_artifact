#pragma once
#include <time.h>
#include <stdio.h>

#define CACHE_LINE 64

typedef struct {
    int             id;
    int             _pad0;    // 4 bytes padding (align timespec to 8)
    struct timespec start;    // 16 bytes
    struct timespec end;      // 16 bytes
    double          result;   // 8 bytes  — stores pi_leibniz output to prevent DCE
    char            _pad1[16]; // pad struct to 64 bytes
} __attribute__((aligned(CACHE_LINE))) task_info_t;
// static_assert(sizeof(task_info_t) == CACHE_LINE)

static inline double pi_leibniz(long iters) {
    //printf("Computing pi with %ld iterations...\n", iters);
    double sum = 0.0, sign = 1.0;
    for (long k = 0; k < iters; ++k) {
        sum += sign / (2.0 * (double)k + 1.0);
        sign = -sign;
    }
    return 4.0 * sum;
}

// Implemented separately per runtime in tasks_oss.c / tasks_omp.c
void submit_tasks(task_info_t *info, long ntasks, long niters);
