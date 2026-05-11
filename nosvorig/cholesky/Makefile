CC=clang
MCC=clang
MPICC=mpicc
MCC_WRAPPERS=MPICH_CC=$(MCC) OMPI_CC=$(MCC) I_MPI_CC=$(MCC)

OSS_FLAGS=-fompss-2=libnodes

# BLAS/LAPACK libraries
# Adjust these based on your system (MKL or OpenBLAS)
#BLAS_LIB=-lcblas -llapacke -lm
# For MKL, uncomment and adjust:
#BLAS_LIB=-L$(MKLROOT)/lib/intel64 -lmkl_intel_lp64 -lmkl_sequential -lmkl_core -lpthread -lm -ldl
#BLAS_FLAGS=-DUSE_MKL -I$(MKLROOT)/include
BLAS_LIB=-lcblas
BLAS_FLAGS=-DUSE_BLIS -I$(BLIS_HOME)/include/blis
LIBFLAME_LIB=-L$(LIBFLAME_HOME)/lib -lflame
LIBFLAME_FLAGS=-DUSE_BLIS -I$(LIBFLAME_HOME)/include

OVNI_INCLUDE=-I$(OVNI_HOME)/include
OVNI_LIB=-L$(OVNI_HOME)/lib -lovni

SYS_LIB=-lm -lnuma -lvalloc

TARGET_BASE=cholesky

################### Derived Quantities (no modification required) ##############

CFLAGS=-D_GNU_SOURCE -O3 -g -ffast-math -march=native -mtune=native $(OVNI_INCLUDE)

LIB_PATHS=$(SYS_LIB) $(BLAS_LIB) $(LIBFLAME_LIB) $(OVNI_LIB)

OBJ_DIR := obj

SRC_FILE := cholesky.c

# Object files for different versions
OBJ_OSS := $(OBJ_DIR)/cholesky-oss.o

.PHONY: all
all: $(TARGET_BASE)_oss.bin

# Create obj directory if it doesn't exist
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# OmpSs-2 Object Files
$(OBJ_OSS): $(SRC_FILE) | $(OBJ_DIR)
	$(MCC) $(CFLAGS) $(OSS_FLAGS) $(BLAS_FLAGS) $(LIBFLAME_FLAGS) -c $< -o $@

# Link binaries
$(TARGET_BASE)_oss.bin: $(OBJ_OSS)
	$(MCC) $(CFLAGS) $(OSS_FLAGS) $^ -o $@ $(LIB_PATHS)

.PHONY: clean
clean:
	@rm -f $(OBJ_DIR)/* $(TARGET_BASE)*.bin

.PHONY: help
help:
	@echo "Available targets:"
	@echo "  all                  - Build OmpSs-2 version (default)"
	@echo "  $(TARGET_BASE)_oss.bin  - Build OmpSs-2 version"
	@echo "  clean                - Remove object files and binaries"
	@echo ""
	@echo "Note: Adjust BLAS_LIB and BLAS_FLAGS for your system"
	@echo "      (MKL vs OpenBLAS/reference BLAS)"
