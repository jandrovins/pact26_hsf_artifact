#include <stdint.h>
#include <stdio.h>
#include "heat.h"

#ifndef SIMD
void computeBlock(const int64_t rows, const int64_t cols,
		const int rstart, const int rend,
		const int cstart, const int cend,
		double M[rows][cols])
{
	for (int r = rstart; r <= rend; ++r) {
		for (int c = cstart; c <= cend; ++c) {
			M[r][c] = 0.25*(M[r-1][c] + M[r+1][c] + M[r][c-1] + M[r][c+1]);
		}
	}
}
#else
void computeBlock(const int64_t rows, const int64_t cols,
		const int rstart, const int rend,
		const int cstart, const int cend,
		double M[rows][cols])
{
	(void) cend;
	// Assuming square blocks
	const int bs = rend-rstart+1;
	for (int k = 0; k < bs; ++k) {
		#pragma omp simd
		for (int j = 0; j <= k; ++j) {
			const int rr = rstart+k-j;
			const int cc = cstart+j;
			M[rr][cc] = 0.25*(M[rr-1][cc] + M[rr+1][cc] + M[rr][cc-1] + M[rr][cc+1]);
		}
	}
	for (int k = bs-2; k >= 0; --k) {
		#pragma omp simd
		for (int j = 0; j <= k; ++j) {
			const int rr = rstart+bs-j-1;
			const int cc = cstart+bs+j-k-1;
			M[rr][cc] = 0.25*(M[rr-1][cc] + M[rr+1][cc] + M[rr][cc-1] + M[rr][cc+1]);
		}
	}
}
#endif


#if 1

void computeBlockResidual(const int64_t rows, const int64_t cols,
		const int rstart, const int rend,
		const int cstart, const int cend,
		double M[rows][cols], double relax,
		double *residual, double *max_elem)
{
	//double relax = 1.95;
	for (int r = rstart; r <= rend; ++r) {
		for (int c = cstart; c <= cend; ++c) {
			double old = M[r][c];
			double fdiff = 0.25*(M[r-1][c] + M[r+1][c] + M[r][c-1] + M[r][c+1]);
			double new = (1 - relax) * old + relax * fdiff;
			double diff = new - old;
			/* Use the largest absolute error as residual */
			*residual = fmax(*residual, fabs(diff));
			*max_elem = fmax(*max_elem, fabs(new));
			//fprintf(stderr, "residual = %e in (%4d, %4d)\n", residual, r, c);
			M[r][c] = new;
		}
	}
}

#else

/* Red-black parallelization */
void computeBlockResidual(const int64_t rows, const int64_t cols,
		const int rstart, const int rend,
		const int cstart, const int cend,
		double M[rows][cols], double relax,
		double * restrict residual, double * restrict max_elem)
{
	(void)(residual);
	(void)(max_elem);

	const double A = 1 - relax;
	const double B = 0.25 * relax;

	//double relax = 1.95;
	for (int r = rstart; r <= rend; r += 2) {
		#pragma clang loop vectorize(enable)
		for (int c = cstart; c <= cend; c += 2) {
			double old = M[r][c];
			double new = A * old + B * (M[r-1][c] + M[r+1][c] + M[r][c-1] + M[r][c+1]);
			//double diff = new - old;
			/* Use the largest absolute error as residual */
			//*residual = fmax(*residual, fabs(diff));
			//*max_elem = fmax(*max_elem, fabs(new));
			//fprintf(stderr, "residual = %e in (%4d, %4d)\n", residual, r, c);
			M[r][c] = new;
		}
	}

	for (int r = rstart+1; r <= rend; r += 2) {
		#pragma clang loop vectorize(enable)
		for (int c = cstart+1; c <= cend; c += 2) {
			double old = M[r][c];
			double new = A * old + B * (M[r-1][c] + M[r+1][c] + M[r][c-1] + M[r][c+1]);
			//double diff = new - old;
			/* Use the largest absolute error as residual */
			//*residual = fmax(*residual, fabs(diff));
			//*max_elem = fmax(*max_elem, fabs(new));
			//fprintf(stderr, "residual = %e in (%4d, %4d)\n", residual, r, c);
			M[r][c] = new;
		}
	}
}

#endif
