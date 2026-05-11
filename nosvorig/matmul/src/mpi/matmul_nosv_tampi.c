#include "common/matmul.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <mpi.h>
#include <TAMPI.h>
#include <nosv.h>

#ifdef USE_MKL
#include <mkl.h>
#else
#include <cblas.h>
#endif

nosv_task_t main_task;

void matmul_init(void)
{
	int ret;

	if ((ret = nosv_init())) {
		fprintf(stderr, "nosv_init failed: %s\n",
				nosv_get_error_string(ret));
		exit(EXIT_FAILURE);
	}

	if ((ret = nosv_attach(&main_task, NULL, "main_matmul_attached",
					NOSV_ATTACH_NONE))) {
		fprintf(stderr, "nosv_attach failed: %s\n",
				nosv_get_error_string(ret));
		exit(EXIT_FAILURE);
	}
}

void matmul_finish(void)
{
	int ret;

	if ((ret = nosv_detach(NOSV_DETACH_NONE))) {
		fprintf(stderr, "nosv_detach failed: %s\n",
				nosv_get_error_string(ret));
		exit(EXIT_FAILURE);
	}

	if ((ret = nosv_shutdown())) {
		fprintf(stderr, "nosv_shutdown failed: %s\n",
				nosv_get_error_string(ret));
		exit(EXIT_FAILURE);
	}
}

static void matmul(size_t TS, double (*A)[TS], double (*B)[TS], 
		double (*C)[TS], const double *Alpha, const double *Beta)
{
	// Launch dgemm kernel 
	cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
		TS, TS, TS, *Alpha, (const double *) A, TS, (const double *) B, TS, *Beta, (double *) C, TS);
}

static void tampi_sendrecv(const void *sendbuff, int dst, void *recvbuff, int src, size_t size, int tag)
{
#if (TAMPI_VERSION_MAJOR == 3)
	MPI_Request requests[2];
	MPI_Isend(sendbuff, size, MPI_BYTE, dst, tag, MPI_COMM_WORLD, &requests[0]);
	MPI_Irecv(recvbuff, size, MPI_BYTE, src, tag, MPI_COMM_WORLD, &requests[1]);
	MPI_Waitall(2, requests, MPI_STATUSES_IGNORE);
#elif (TAMPI_VERSION_MAJOR == 4)
	MPI_Send(sendbuff, size, MPI_BYTE, dst, tag, MPI_COMM_WORLD);
	MPI_Recv(recvbuff, size, MPI_BYTE, src, tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
#else
	#error "TAMPI version not supported for this benchmark"
#endif
}

static void copy_tile(void *dstbuff, const void *srcbuff, size_t size)
{
	memcpy(dstbuff, srcbuff, size);
}

void matmul_solve(
	size_t N, size_t M, size_t TS,
	matmul_t *mm,
	size_t timesteps
) {
	const size_t tileSize = TS*TS*sizeof(double);

	// Matrix dimensions:
	//   N = input; M = N / nranks
	//   Matrix A: M x N (distributed by rows)
	//   Matrix B: N x M (distributed by cols)
	//   Matrix C: N x M (distributed by cols)

	double (*A)[N/TS][TS][TS] = (double (*)[N/TS][TS][TS])mm->A;
	double (*remote1)[N/TS][TS][TS] = (double (*)[N/TS][TS][TS])mm->remote1;
	double (*remote2)[N/TS][TS][TS] = (double (*)[N/TS][TS][TS])mm->remote2;
	double (*B)[M/TS][TS][TS] = (double (*)[M/TS][TS][TS])mm->B;
	double (*C)[M/TS][TS][TS] = (double (*)[M/TS][TS][TS])mm->C;
	double *Alpha = &(mm->alpha);
	double *Beta = &(mm->beta);

	// Compute neighboring ranks (ring)
	const int dst = (rank > 0) ? rank-1 : nranks-1;
	const int src = (rank < nranks-1) ? rank+1 : 0;

	for (size_t t = 0; t < timesteps; ++t) {
		// Start computing the part of matrix C that we can compute
		// with the part of matrix A that we initially have
		size_t offC = rank * M/TS;
		double (*sendA)[N/TS][TS][TS] = A;
		double (*recvA)[N/TS][TS][TS] = remote1;

		// Compute matrix C partially and circulate the A matrix
		// with the neighboring ranks following a ring
		for (int r = 0; r < nranks; ++r) {
			// Spawn the matmul kernels of each tile
			for (size_t k = 0; k < N/TS; ++k) {
				for (size_t i = 0; i < M/TS; ++i) {
					for (size_t j = 0; j < M/TS; ++j) {
						matmul(TS, A[i][k], B[k][j], C[offC+i][j], Alpha, Beta);
					}
				}
			}
			if (r < nranks-1) {
				// Send the current part of A, receive the next part, and update
				// the copy of A in the device
				for (size_t i = 0; i < M/TS; ++i) {
					for (size_t j = 0; j < N/TS; ++j) {
						tampi_sendrecv(sendA[i][j], dst, recvA[i][j], src, tileSize, i*(N/TS)+j);
						copy_tile(A[i][j], recvA[i][j], tileSize);
					}
				}
			}

			// Move to the next part of matrix C
			offC = (offC + M/TS) % (N/TS);

			// Swap pointers of send and receive buffers
			void *aux = recvA;
			recvA = (r != 0) ? sendA : remote2;
			sendA = aux;
		}
	}
}
