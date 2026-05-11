
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

#ifdef USING_MPI  // Compile this routine only if running in parallel
#include <iostream>
using std::cerr;
using std::endl;
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <cassert>
#include <cmath>
#include <climits>

#include "constants.h"
#ifdef USING_INTEROP
#include <TAMPI.h>
extern size_t CHUNK_SIZE;
#endif

#include "exchange_externals.hpp"
#undef DEBUG

#include "TaskGroupManager.hpp"

void exchange_externals(HPC_Sparse_Matrix *A, const double *x, std::vector<int> &fsb_dep_indexes)
{
	int i, j, k;
	int num_external = 0;

	// Extract Matrix pieces

	int local_nrow = A->local_nrow;
	int local_ncol = A->local_ncol;
	int num_neighbors = A->num_send_neighbors;
	int *recv_length = A->recv_length;
	int *send_length = A->send_length;
	int *neighbors = A->neighbors;
	double *send_buffer = A->send_buffer;
	int total_to_be_sent = A->total_to_be_sent;
	long long *elements_to_send = A->elements_to_send;

	int size, rank; // Number of MPI processes, My process ID

	MPI_Comm_size(MPI_COMM_WORLD, &size);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);

#ifdef USING_MPI
	int chunk = (local_ncol - local_nrow)/NUM_COMM_PART;
#endif

	//
	//  first post receives, these are immediate receives
	//  Do not wait for result to come, will do that at the
	//  wait call below.
	//

	int MPI_MY_TAG = 99;

#ifndef USING_MPI
	MPI_Request *request = new MPI_Request[num_neighbors];
#endif

	//
	// Externals are at end of locals
	//
	double *x_external = (double *)x + local_nrow;

	//
	// Fill up send buffer
	//

#ifdef USING_MPI
    if (num_neighbors > 0) {
    //#pragma oss task in(x[elements_to_send[0] : elements_to_send[total_to_be_sent - 1]]) out(send_buffer[0 : total_to_be_sent - 1]) label(Fill send_buffer)
	#ifdef USING_OSS
		#pragma oss task priority(INT_MAX) in({x[fsb_dep_indexes[elem]], elem=0;fsb_dep_indexes.size()}) \
						out(send_buffer[0], {send_buffer[send_length[s-1]], s=1;num_neighbors}) \
						label("Fill send_buffer")
	#elif defined(USING_OMP)
		#pragma omp task depend(iterator(elem=0:fsb_dep_indexes.size()), in: x[fsb_dep_indexes[elem]]) \
						depend(iterator(s=1:num_neighbors), out: send_buffer[0], send_buffer[send_length[s-1]])
	#endif
#endif
        for (i = 0; i < total_to_be_sent; i++) {
            send_buffer[i] = x[elements_to_send[i]];
        }
#ifdef USING_MPI
    }
#endif

	// Post receives first
	for (i = 0; i < num_neighbors; i++) {
		int n_recv = recv_length[i];
		for (int g = 0, j = 0; g < n_recv; g += chunk, ++j) {
			int to = g + chunk > n_recv ? n_recv : g + chunk;
			//#pragma oss task out(x_external[g : to - 1]) label(Recv data)
			#ifdef USING_OSS
				#pragma oss task taskgroup(tg_manager.get_flat_taskgroup(i)) priority(INT_MAX) out(x_external[g]) label("Recv data")
			#elif defined(USING_OMP)
				#pragma omp task depend(out: x_external[g])
			#endif
			{
				MPI_Request request;
				
				#ifdef USING_INTEROP
				TAMPI_Irecv(&x_external[g], to - g, MPI_DOUBLE, neighbors[i], MPI_MY_TAG + j,
					MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				#else
				MPI_Irecv(&x_external[g], to - g, MPI_DOUBLE, neighbors[i], MPI_MY_TAG + j,
					MPI_COMM_WORLD, &request);

				MPI_Status status;
				while (true) {
					int flag = 0;
					MPI_Test(&request, &flag, &status);
					if (flag) break;
					// Sleep for 1 ms
					//nosv_waitfor(1e6, NULL);
					nosv_yield(NOSV_YIELD_NONE);
				}
				#endif
			}
		}
		x_external += n_recv;
	}

	//
	// Send to each neighbor
	//

	for (i = 0; i < num_neighbors; i++) {
		int n_send = send_length[i];
		for (int g = 0, j = 0; g < n_send; g += chunk, ++j) {
			int to = g + chunk > n_send ? n_send : g + chunk;
			//#pragma oss task in(send_buffer[g : to - 1]) label(Send data)
			#pragma oss task taskgroup(tg_manager.get_flat_taskgroup(i)) priority(INT_MAX) in(send_buffer[g]) label("Send data")
			{
				MPI_Request request;
				#ifdef USING_INTEROP
				TAMPI_Isend(&send_buffer[g], to - g, MPI_DOUBLE, neighbors[i], MPI_MY_TAG + j,
					 MPI_COMM_WORLD);
				#else
				MPI_Send(&send_buffer[g], to - g, MPI_DOUBLE, neighbors[i], MPI_MY_TAG + j,
					 MPI_COMM_WORLD);
				#endif
			}
		}
		send_buffer += n_send;
	}

	return;
}
#endif // USING_MPI
