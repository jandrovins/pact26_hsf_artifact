# Compilers
CC=clang
MCC=clang

# Preprocessor flags
# enable SIMD
CPPFLAGS=-DSIMD

# Compiler flags
CFLAGS=-march=native -O3 -ffast-math -std=gnu11 -g3  -fopenmp-simd
MCCFLAGS=$(CFLAGS) -fompss-2=libnodes -I${TGLIB_HOME}/include

# Kernel flags
KFLAGS=$(CFLAGS) -fopenmp-simd

# Linker flags
LDFLAGS=-lrt -lm -lovni -L${TGLIB_HOME}/lib -lvalloc -lnuma

all: 02.heat_ompss2_prio.bin

02.heat_ompss2_prio.bin: misc.c main_smp_prio.c
	$(MCC) $(CPPFLAGS) $(MCCFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f *.o *.bin
