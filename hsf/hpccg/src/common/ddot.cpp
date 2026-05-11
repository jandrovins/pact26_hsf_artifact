//@HEADER
// ************************************************************************
//
//               HPCCG: Simple Conjugate Gradient Benchmark Code
//                 Copyright (2006) Sandia Corporation
//
// Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive
// license for use of this work by or on behalf of the U.S. Government.
//
// BSD 3-Clause License
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Michael A. Heroux (maherou@sandia.gov)
//
// ************************************************************************
//@HEADER

/////////////////////////////////////////////////////////////////////////

// Routine to compute the dot product of two vectors where:

// n - number of vector elements (on this processor)

// x, y - input vectors

// residual - pointer to scalar value, on exit will contain result.

/////////////////////////////////////////////////////////////////////////

#include "ddot.hpp"
#ifdef USING_INTEROP
#include <TAMPI.h>
#endif
#include <iostream>
int ddot(const int n, const double *const x, const double *const y,
	 double *const result, double & time_allreduce)
{
	abort(); // this is not used....
	double local_result = 0.0;

	if (y == x) {
//#ifdef USING_OMP
//#pragma omp parallel for schedule(static) reduction (+:local_result)
//#endif
		for (int i = 0; i < n; i++) local_result += x[i] * x[i];
    }
	else {
//#ifdef USING_OMP
//#pragma omp parallel for schedule(static) reduction (+:local_result)
//#endif
		for (int i = 0; i < n; i++) local_result += x[i] * y[i];
    }

#ifdef USING_MPI
	// Use MPI's reduce function to collect all partial sums
	double t0 = mytimer();
	double global_result = 0.0;
	#ifdef USING_INTEROP
	TAMPI_Iallreduce(&local_result, &global_result, 1, MPI_DOUBLE, MPI_SUM,
				MPI_COMM_WORLD);
	#else
	MPI_Allreduce(&local_result, &global_result, 1, MPI_DOUBLE, MPI_SUM,
				MPI_COMM_WORLD);
	#endif
	// TODO: If this becomes being used, *result = global_result should be done 
	// outside the task, bc. TAMPI_Iallreduce returns immediately (?)
	*result = global_result;
	time_allreduce += mytimer() - t0;
#else
	*result = local_result;
#endif

	return 0;
}
