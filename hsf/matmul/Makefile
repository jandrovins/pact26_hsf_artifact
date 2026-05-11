BUILDDIR?=.

CC = clang
MPICC = mpicc
WRAPPERS = MPICH_CC=$(CC) I_MPI_CC=$(CC) OMPI_CC=$(CC)

ifdef I_MPI_ROOT
MPICC=mpiicc
endif

#BLAS_LIB=-L$(MKLROOT)/lib/intel64 -lmkl_intel_lp64 -lmkl_sequential -lmkl_core -lpthread -lm -ldl
#BLAS_FLAGS=-DUSE_MKL -I$(MKLROOT)/include
BLAS_LIB=-lcblas
BLAS_FLAGS=-DUSE_BLIS -I$(BLIS_HOME)/include/blis

CPPFLAGS=-Isrc

OSS_FLAGS=-fompss-2=libnodes
CFLAGS=-D_GNU_SOURCE -g -std=c11 -O3 -ffast-math -march=native -mtune=native $(OSS_FLAGS) $(BLAS_FLAGS)

LIBS=-lrt -lm $(BLAS_LIB) -ltglib


TAMPI_CPPFLAGS=-I$(TAMPI_HOME)/include -DTAMPI
TAMPI_LDFLAGS=-ltampi-c -L$(TAMPI_HOME)/lib -Wl,--enable-new-dtags -Wl,-rpath=$(TAMPI_HOME)/lib

BIN= \
	$(BUILDDIR)/02.matmul_ompss2_itampi.bin

all: $(BUILDDIR) $(BIN)

$(BUILDDIR)/02.matmul_ompss2_itampi.bin: src/common/matmul.c src/mpi/main.c src/mpi/matmul_ompss2_itampi.c
	$(WRAPPERS) $(MPICC) $(CPPFLAGS) $(CFLAGS) $(TAMPI_CPPFLAGS) $^ -o $@ $(TAMPI_LDFLAGS) $(LIBS)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -f $(BUILDDIR)/*.o $(BUILDDIR)/*.bin
