#ifndef MATMUL_H
#define MATMUL_H

#include <stddef.h>
#include <tglib.h>
#include <stdlib.h>
#include <stdio.h>
#include <nosv.h>
#include <nosv/affinity.h>

extern int rank;
extern int nranks;

typedef struct {
	size_t n; /* number of columns of output matrix */
	size_t m; /* number of rows of the output matrix (in total) */
	size_t ts; /* tile size (size of block size) */
	size_t timesteps; /* iterations */
	size_t warmup; /* iterations of warmup */
} matmul_conf_t;

typedef struct {
	void *A;
	void *remote1;
	void *remote2;
	void *B;
	void *C;

	double alpha;
	double beta;

	size_t n;
	size_t m;
	size_t m_per_rank;
	size_t ts;

	/* 2D superblock taskgroups */
	nosv_task_group_t *block_tgs;     /* flat array [num_sb_rows * num_sb_cols] */
	size_t num_block_tgs;
	nosv_task_group_t *region_tgs;    /* parent TGs for two-level hierarchy (NULL if flat) */
	size_t num_region_tgs;
	size_t sr;                        /* tile-rows per superblock */
	size_t sc;                        /* tile-cols per superblock */
	size_t num_sb_rows;               /* number of superblock rows */
	size_t num_sb_cols;               /* number of superblock cols */
	int use_hierarchy;                /* 0 = flat, 1 = two-level */
} matmul_t;

double matmul_gettime(void);
void matmul_fail(const char *msg);
void matmul_getconf(int argc, char **argv, matmul_conf_t *conf, int nranks);
void matmul_setup(matmul_conf_t *conf, matmul_t *matmul, size_t m_per_rank);
int matmul_check(size_t N, size_t M, size_t TS, double (*A)[M/TS][TS][TS], double expected);
void matmul_solve(size_t N, size_t M, size_t TS, matmul_t *mm, size_t timesteps);
void matmul_barrier_issue(size_t N, size_t M, size_t TS, matmul_t *matmul);
void matmul_barrier_notify(void);
void matmul_init(void);
void matmul_finish(void);
void matmul_report(double t, matmul_conf_t *conf);

static inline nosv_task_group_t matmul_get_taskgroup(matmul_t *mm, size_t k, size_t i, size_t j)
{
	(void)k;
	if (mm->num_block_tgs == 0)
		return NULL;

	size_t bi = i / mm->sr;
	size_t bj = j / mm->sc;
	if (bi >= mm->num_sb_rows) bi = mm->num_sb_rows - 1;
	if (bj >= mm->num_sb_cols) bj = mm->num_sb_cols - 1;
	size_t idx = bi * mm->num_sb_cols + bj;
	return mm->block_tgs[idx];
}

#endif
