# Compiler and flags

CC = clang
OSS_CXX = clang++
OSS_CXXFLAGS = -O3 -DSIMD -D_POSIX_C_SOURCE=200809L -DNDEBUG -fopenmp-simd \
	-fompss-2=libnodes \
	-I$(OVNI_HOME)/include \
	-std=c++17

# Target executables
OSS_TARGET = b6_multisaxpy_nodes

# Source files
OSS_SOURCE = multisaxpy.cpp

# Libraries
LIBS = -pthread -lm -ldl -lpthread \
	-L$(OVNI_HOME)/lib -lovni \
	-ltglib \
	-lnuma

# Default target
all: $(OSS_TARGET)

# Build target
$(OSS_TARGET): $(OSS_SOURCE)
	$(OSS_CXX) $(OSS_CXXFLAGS) $< -o $@ $(LIBS)

# Clean target
clean:
	rm -f $(OSS_TARGET)

.PHONY: all clean
