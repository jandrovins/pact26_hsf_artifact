# @HEADER
# ***********************************************************************
#  
#                HPCCG: Simple Conjugate Gradient Benchmark Code
#                  Copyright (2006) Sandia Corporation
#  
#  Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive
#  license for use of this work by or on behalf of the U.S. Government.
#  
#  BSD 3-Clause License
#  
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions are met:
#  
#  * Redistributions of source code must retain the above copyright notice, this
#    list of conditions and the following disclaimer.
#  
#  * Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#  
#  * Neither the name of the copyright holder nor the names of its
#    contributors may be used to endorse or promote products derived from
#    this software without specific prior written permission.
#  
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
#  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
#  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
#  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
#  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
#  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
#  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
#  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
#  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#  
#  Questions? Contact Michael A. Heroux (maherou@sandia.gov) 
#  
#  ************************************************************************

CXX=clang++
MCXX=clang++
MPIMCXX=mpicxx
MCXX_WRAPPERS=MPICH_CXX=$(MCXX) OMPI_CXX=$(MCXX) I_MPI_CXX=$(MCXX)

TAMPI_DIR=$(TAMPI_HOME)
TAMPI_LIB=-L/$(TAMPI_DIR)/lib -ltampi-c
TAMPI_INC=-I$(TAMPI_DIR)/include

USE_INTEROP = -DUSING_MPI -DUSING_INTEROP $(TAMPI_INC)
NOT_USE_INTEROP = -DUSING_MPI

OSS_FLAGS=-DUSING_OSS -fompss-2=libnodes
OMP_ORIG_FLAGS=-DUSING_MPI -DUSING_OMP -fopenmp
OMP_OMPV_FLAGS=$(USE_INTEROP) -DUSING_OMP -fopenmp=libompv

SYS_LIB =-lm -lnuma -ltglib

TARGET_BASE = HPCCG_mpi

################### Derived Quantities (no modification required) ##############

CXXFLAGS=-D_GNU_SOURCE -Iinclude -DWALL -O3 -ggdb3 -fno-omit-frame-pointer -ffast-math -march=native -mtune=native

LIB_PATHS= $(SYS_LIB)

INC_PATHS= include

OBJ_DIR := obj

SRC_INTEROP_DIR := src/mpi_interop_ompss
SRC_INTEROP_FILES := $(wildcard $(SRC_INTEROP_DIR)/*.cpp)
# One set of .obj files per binary
INTEROP_OMPSS_NOTAMPI_FILES := $(patsubst $(SRC_INTEROP_DIR)/%.cpp,$(OBJ_DIR)/%-mpi-oss-notampi.o,$(SRC_INTEROP_FILES))

COMMON_DIR := src/common
COMMON_FILES := $(wildcard $(COMMON_DIR)/*.cpp)

# One set of .obj files per binary
COMMON_OMPSS_NOTAMPI_FILES := $(patsubst $(COMMON_DIR)/%.cpp,$(OBJ_DIR)/%-common-oss-notampi.o,$(COMMON_FILES))
COMMON_OMPSS_TAMPI_FILES := $(patsubst $(COMMON_DIR)/%.cpp,$(OBJ_DIR)/%-common-oss-tampi.o,$(COMMON_FILES))
COMMON_OMPORIG_FILES := $(patsubst $(COMMON_DIR)/%.cpp,$(OBJ_DIR)/%-common-omporig.o,$(COMMON_FILES))
COMMON_OMPV_FILES := $(patsubst $(COMMON_DIR)/%.cpp,$(OBJ_DIR)/%-common-ompv.o,$(COMMON_FILES))

.PHONY: all
all: $(TARGET_BASE)_oss-notampi.bin

# Create obj directory if it doesn't exist
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# OSS-NOTAMPI Object Files
$(OBJ_DIR)/%-mpi-oss-notampi.o: $(SRC_INTEROP_DIR)/%.cpp | $(OBJ_DIR)
	$(MCXX_WRAPPERS) $(MPIMCXX) $(CXXFLAGS) $(NOT_USE_INTEROP) $(OSS_FLAGS) -c $< -o $@

$(OBJ_DIR)/%-common-oss-notampi.o: $(COMMON_DIR)/%.cpp | $(OBJ_DIR)
	$(MCXX_WRAPPERS) $(MPIMCXX) $(CXXFLAGS) $(NOT_USE_INTEROP) $(OSS_FLAGS) -c $< -o $@

# Link binaries
$(TARGET_BASE)_oss-notampi.bin: $(INTEROP_OMPSS_NOTAMPI_FILES) $(COMMON_OMPSS_NOTAMPI_FILES)
	$(MCXX_WRAPPERS) $(MPIMCXX) $(CXXFLAGS) $(NOT_USE_INTEROP) $(OSS_FLAGS) \
		$^ -o $@ $(LIB_PATHS)

.PHONY: clean
clean:
	@rm -f obj/* $(TARGET_BASE)*.bin
