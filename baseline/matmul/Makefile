# Target arch: defaults to native; the pipeline exports HSF_ARCH=znver4 on Genoa-X.
HSF_ARCH ?= native

CC=clang

# BLIS bundles the CBLAS interface (cblas_dgemm) inside libblis-mt.
BLAS_LIB=-L$(BLIS_HOME)/lib -lblis-mt
BLAS_FLAGS=-DUSE_BLIS -I$(BLIS_HOME)/include/blis

# -D_GNU_SOURCE exposes CPU_ZERO/CPU_ISSET/sched_getaffinity (sched.h) used by
# the USE_NUMA_INTERLEAVE affinity code.
CFLAGS=-ffast-math -march=$(HSF_ARCH) -mtune=$(HSF_ARCH) -O3 -fopenmp -D_GNU_SOURCE -DUSE_CBLAS $(BLAS_FLAGS)
LDFLAGS=$(BLAS_LIB)

NUMA_FLAGS=-DUSE_NUMA_INTERLEAVE -lnuma

mt-dgemm: mt-dgemm.c
	$(CC) $(CFLAGS) -o mt-dgemm mt-dgemm.c $(LDFLAGS)

numa: mt-dgemm.c
	$(CC) $(CFLAGS) -DUSE_NUMA_INTERLEAVE -o mt-dgemm mt-dgemm.c $(LDFLAGS) -lnuma

clean:
	rm mt-dgemm *.o

