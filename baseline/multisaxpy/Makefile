CXX    = clang++
MPICXX = mpicxx

CXXFLAGS = -O3 -g -fopenmp
LDFLAGS  = -fopenmp

SMP_TARGET = multisaxpy_smp
MPI_TARGET = multisaxpy_mpi

all: $(SMP_TARGET) $(MPI_TARGET)

$(SMP_TARGET): multisaxpy-smp.cpp
	$(CXX) $(CXXFLAGS) -DBENCH6_NAME=\"$(SMP_TARGET)\" -o $@ $< -lnuma

$(MPI_TARGET): multisaxpy-mpi.cpp
	$(MPICXX) $(CXXFLAGS) -DBENCH6_NAME=\"$(MPI_TARGET)\" -o $@ $< -lnuma

clean:
	rm -f $(SMP_TARGET) $(MPI_TARGET)

.PHONY: all clean
