#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/time.h>

static void initialize(long const N, long const M, long const TS,
		double (* restrict A)[M/TS][TS][TS], double value) 
{
    long const NB_I = N / TS;
    long const NB_J = M / TS;

    for (long i = 0; i < NB_I; i++) {
        for (long j = 0; j < NB_J; j++) {
            #pragma oss task out(A[i][j]) label("initialize block")
            for (long ii = 0; ii < TS; ii++) {
                for (long jj = 0; jj < TS; jj++) {
                    A[i][j][ii][jj] = value;
                }
            }
        }
    }
    #pragma oss taskwait
}


static void matmul(const long N, const long M, const long P, const long TS,
		double (*restrict A)[M/TS][TS][TS],
		double (*restrict B)[P/TS][TS][TS],
		double (*restrict C)[P/TS][TS][TS]) 
{
    long const NB_I = N / TS;
    long const NB_J = P / TS;
    long const NB_K = M / TS;

    for (long k = 0; k < NB_K; k++) {
        for (long i = 0; i < NB_I; i++) {
            for (long j = 0; j < NB_J; j++) {
                double (* restrict A_block)[TS] = A[i][k];
                double (* restrict B_block)[TS] = B[k][j];
                double (* restrict C_block)[TS] = C[i][j];

                #pragma oss task in([TS]A_block) in ([TS]B_block) inout([TS]C_block) label("matmul block")
                for (long ii = 0; ii < TS; ii++) {
                    for (long jj = 0; jj < TS; jj++) {
                        for (long kk = 0; kk < TS; kk++) {
                            C_block[ii][jj] += A_block[ii][kk] * B_block[kk][jj];
                        }
                    }
                }
            }
        }
    }
    #pragma oss taskwait
}


static bool verify(long const N, long const M, long const TS,
		double value, double (* restrict C)[M/TS][TS][TS]) 
{
    long const NB_I = N / TS;
    long const NB_J = M / TS;

    bool validates = true;

    for (long i = 0; i < NB_I; i++) {
        for (long j = 0; j < NB_J; j++) {
            #pragma oss task in(C[i][j]) label("verify block") inout(validates)
            for (long ii = 0; ii < TS; ii++) {
                for (long jj = 0; jj < TS; jj++) {
                    if (C[i][j][ii][jj] != value) {
                        validates = false;
                        break;
                    }
                }
            }
        }
    }
    #pragma oss taskwait

    return validates;
}

int main(int argc, char* argv[]) 
{
   struct timeval start;
   struct timeval stop;
   unsigned long elapsed;

   if (argc != 5) {
      fprintf(stderr, "Usage: %s <N> <M> <P> <BLOCK_SIZE>\n", argv[0]);
      return 1;
   }

   long N = atol(argv[1]);
   long M = atol(argv[2]);
   long P = atol(argv[3]);
   long TS = atol(argv[4]);

   if (N%TS != 0L) {
      printf("Error: TS (%ld) must divide N (%ld)\n", TS, N);
      return 1;
   }
   else if (M%TS != 0L) {
      printf("Error: TS (%ld) must divide M (%ld)\n", TS, M);
      return 1;
   }
   else if (P%TS != 0L) {
      printf("Error: TS (%ld) must divide P (%ld)\n", TS, P);
      return 1;
   }

   double (*A)[M/TS][TS][TS] = malloc(sizeof(double)*N*M);
   double (*B)[P/TS][TS][TS] = malloc(sizeof(double)*M*P);
   double (*C)[P/TS][TS][TS] = malloc(sizeof(double)*N*P);

   if (A == NULL || B == NULL || C == NULL)
   {
      printf("Error: Can't allocate %ld Bytes\nExiting.\n", (N*M + M*P + N*P)*sizeof(double));
      exit(1);
   }

   initialize(N, M, TS, A, 1.0);
   initialize(M, P, TS, B, 2.0);
   initialize(N, P, TS, C, 3.0);

   gettimeofday(&start, NULL);

   matmul(N, M, P, TS, A, B, C);

   gettimeofday(&stop, NULL);

   bool validates = verify(N, P, TS, 3.0 + 2.0*1.0*M, C);

   elapsed = 1000000*(stop.tv_sec - start.tv_sec);
   elapsed += stop.tv_usec - start.tv_usec;

   double performance = N*M*P*2.0;
   performance = performance/(double)elapsed;

   if (validates)
      printf("N, %lu, M, %lu, P, %lu, block size, %lu, MFlops/s, %f, time (ms), %lu\n", N, M, P, TS, performance, elapsed/1000);
   else
      printf("Incorrect result obtained\nExiting.\n");

   return 0;
}
