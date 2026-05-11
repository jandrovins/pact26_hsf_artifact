#pragma once
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double mean, std, min, p25, p50, p75, max;
} stats_t;

static int stats__cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void stats_compute(double *samples, long n, stats_t *out) {
    double *sorted = malloc(n * sizeof(double));
    if (!sorted) { perror("malloc"); exit(1); }
    memcpy(sorted, samples, n * sizeof(double));
    qsort(sorted, n, sizeof(double), stats__cmp_double);

    double mean = 0.0;
    for (long i = 0; i < n; i++) mean += samples[i];
    mean /= (double)n;

    double var = 0.0;
    for (long i = 0; i < n; i++) {
        double d = samples[i] - mean;
        var += d * d;
    }

    out->mean = mean;
    out->std  = sqrt(var / (double)n);
    out->min  = sorted[0];
    out->p25  = sorted[(long)(n * 0.25)];
    out->p50  = sorted[(long)(n * 0.50)];
    out->p75  = sorted[(long)(n * 0.75)];
    out->max  = sorted[n - 1];

    free(sorted);
}

static void stats_print(stats_t *s) {
    printf("mean=%.3fus  std=%.3fus  min=%.3fus  p25=%.3fus  p50=%.3fus  p75=%.3fus  max=%.3fus\n",
           s->mean, s->std, s->min, s->p25, s->p50, s->p75, s->max);
}
