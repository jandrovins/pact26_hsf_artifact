#include "common.h"
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NRUNS 100

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <niters>\n", argv[0]);
        return 1;
    }
    long niters = atol(argv[1]);

    double samples[NRUNS];
    for (int i = 0; i < NRUNS; i++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        volatile double r = pi_leibniz(niters);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        (void)r;
        long ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
        samples[i] = ns / 1e3;
    }

    stats_t st;
    stats_compute(samples, NRUNS, &st);

    printf("niters=%ld  ", niters);
    stats_print(&st);
    return 0;
}
