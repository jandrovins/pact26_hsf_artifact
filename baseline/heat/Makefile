# Compilers
CC=clang

# Preprocessor flags (enable SIMD kernel)
CPPFLAGS=-DSIMD -DBENCH6_NAME='"heat_omp"'

# Compiler flags
CFLAGS=-march=znver4 -mtune=znver4 -O3 -ffast-math -std=gnu11 -g3 -fopenmp-simd

# Kernel flags (SIMD vectorization analysis)
KFLAGS=$(CFLAGS) -Rpass-analysis=loop-vectorize

# Linker flags
LDFLAGS=-lrt -lm

CPPFLAGS += -DUSE_NUMA_INTERLEAVE -D_GNU_SOURCE
LDFLAGS  += -lnuma

# Sources
SMP_SRC=misc.c main_smp.c solver_openmp.c

all: heat_omp.bin

heat_omp.bin: kernel.o $(SMP_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fopenmp -o $@ $^ $(LDFLAGS)

kernel.o: kernel.c
	$(CC) $(CPPFLAGS) $(KFLAGS) -c -o $@ $^

clean:
	rm -f *.o *.bin
