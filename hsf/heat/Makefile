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
LDFLAGS=-lrt -lm -lovni -L${TGLIB_HOME}/lib -ltglib

# Sources
SMP_SRC=misc.c main_smp.c

all: 02.heat_ompss2.bin

02.heat_ompss2.bin: $(SMP_SRC)
	$(MCC) $(CPPFLAGS) $(MCCFLAGS) -o $@ $^ $(LDFLAGS)

all_proposals: 02.heat_ompss2_v2_imm.bin 02.heat_ompss2_v3_twolevel.bin 02.heat_ompss2_v4_fifo.bin

kernel.o: kernel.c
	$(CC) $(CPPFLAGS) $(KFLAGS) -c -o $@ $^

clean:
	rm -f *.o *.bin
