# Compiler and flags

CC = clang
OSS_CXX = clang++
OSS_CXXFLAGS = -O3 -DSIMD -D_POSIX_C_SOURCE=200809L -DNDEBUG -fopenmp-simd \
	-fompss-2=libnodes \
	-I$(OVNI_HOME)/include \
	-std=c++17

# Target executables
PRIO_TARGET = b6_multisaxpy_prio

# Source files
OSS_SOURCE = multisaxpy-smp.cpp
PRIO_SOURCE = multisaxpy-prio.cpp

# Libraries
LIBS = -pthread -lm -ldl -lpthread \
	-L$(OVNI_HOME)/lib -lovni \
	-lnuma

# Default target
all: $(PRIO_TARGET)

# Build targets
$(PRIO_TARGET): $(PRIO_SOURCE)
	$(OSS_CXX) $(OSS_CXXFLAGS) $< -o $@ $(LIBS) -lvalloc

# Clean target
clean:
	rm -f $(PRIO_TARGET)

.PHONY: all clean
