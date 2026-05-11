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

// Routine to read a sparse matrix, right hand side, initial guess,
// and exact solution (as computed by a direct solver).

/////////////////////////////////////////////////////////////////////////

// nrow - number of rows of matrix (on this processor)

#include <cstddef>
#include <iostream>
using std::cout;
using std::cerr;
using std::endl;
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <unistd.h>
#include <cmath>
#include <numa.h>
#include <atomic>

#include <linux/mempolicy.h>
#include <numaif.h>

#include <tglib.h>


#include "TaskGroupManager.hpp"
#include "generate_matrix.hpp"

extern size_t NUM_TASKS;

#ifdef DEBUG
	int debug = 1;
#else
	int debug = 0;
#endif



void print_mempolicy(const char* prefix, void* addr = NULL, size_t length = 0)
{
	int rank = 0;
	#ifdef USING_MPI
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	#endif
	
	int policy_thread = -1;
	unsigned long nodemask_thread = 0;
	int ret_thread = get_mempolicy(&policy_thread, &nodemask_thread, sizeof(nodemask_thread) * 8, NULL, 0);
	if (ret_thread != 0) {
		cerr << "get_mempolicy failed for thread!" << endl;
		abort();
	}
	
	std::cout << "[Rank " << rank << ", Thread " << pthread_self() << "] " << prefix 
			  << ": thread_policy=" << policy_thread << " thread_nodemask=0x" 
			  << std::hex << nodemask_thread << std::dec;
	
	if (addr != NULL && length > 0) {
		int policy_addr = -1;
		unsigned long nodemask_addr = 0;
		int ret_addr = get_mempolicy(&policy_addr, &nodemask_addr, sizeof(nodemask_addr) * 8, addr, MPOL_F_ADDR);
		if (ret_addr != 0) {
			cerr << "get_mempolicy failed for addr!" << endl;
			abort();
		}
		
		std::cout << " addr_policy=" << policy_addr << " addr_nodemask=0x" 
				  << std::hex << nodemask_addr << std::dec;
	}
	
	std::cout << std::endl;
}



struct idx {
	long long flat;
	long long ix;
	long long iy;
	long long iz;
	int visited;
	long long order;
};
struct idx *debug_map = nullptr;

void save_orig_indices(int nx, int ny, int nz)
{
	long long total_size = nz * ny * nx;
	debug_map = static_cast<struct idx *>(malloc(sizeof(struct idx) * total_size));
	memset(debug_map, 0, sizeof(struct idx) * nz * ny * nx);
	for (long long  iz = 0; iz < nz; iz++) {
		for (long long  iy = 0; iy < ny; iy++) {
			for (long long  ix = 0; ix < nx; ix++) {
				long long curlocalrow = iz * nx * ny + iy * nx + ix;
				if (debug)
					std::cout << "PREV: " << curlocalrow << " " << iz << " " << iy << " " << ix << " " << curlocalrow << std::endl;

				debug_map[curlocalrow].flat = curlocalrow;
				debug_map[curlocalrow].ix = ix;
				debug_map[curlocalrow].iy = iy;
				debug_map[curlocalrow].iz = iz;
				debug_map[curlocalrow].order = curlocalrow;
			}
		}
	}
}

void generate_matrix(int nx, int ny, int nz, HPC_Sparse_Matrix **A, double **x, double **b, double **xexact)

{
	save_orig_indices(nx, ny, nz);

#ifdef USING_MPI
	int size, rank; // Number of MPI processes, My process ID
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#else
	int size = 1; // Serial case (not using MPI)
	int rank = 0;
#endif

	*A = new HPC_Sparse_Matrix; // Allocate matrix struct and fill it
	(*A)->title = 0;

	// Set this bool to true if you want a 7-pt stencil instead of a 27 pt stencil
	bool use_7pt_stencil = false;

	long long local_nrow = nx * ny * nz;                          // This is the size of our subblock
	assert(local_nrow > 0);                                 // Must have something to work with
	long long local_nnz = 27 * local_nrow;                        // Approximately 27 nonzeros per row (except for boundary nodes)

	long long total_nrow = local_nrow * size;                     // Total number of grid points in mesh
	long long total_nnz = 27 * (long long)total_nrow;       // Approximately 27 nonzeros per row (except for boundary nodes)

	long long start_row = local_nrow * rank;                      // Each processor gets a section of a chimney stack domain
	long long stop_row = start_row + local_nrow - 1;
	if (debug) cout << "Process " << rank << " start_row: " << start_row 
					<< " stop_row: " << stop_row << endl;

	// Check for potential overflow in taskSize calculation
	if (local_nrow > LLONG_MAX - NUM_TASKS + 1) {
		cerr << "Error: Overflow would occur in taskSize calculation" << endl;
		abort();
	}
	
	long long taskSize = (local_nrow + NUM_TASKS - 1) / NUM_TASKS;  // Ceiling division without overflow
	
	// Check for potential overflow in num_blocks calculation
	if (local_nrow > LLONG_MAX - taskSize + 1) {
		cerr << "Error: Overflow would occur in num_blocks calculation" << endl;
		abort();
	}
	
	long long num_blocks = (local_nrow + taskSize - 1) / taskSize;  // Ceiling division without overflow
	int chunk = taskSize;
	tg_manager.create_taskgroups(local_nrow);

	(*A)->nnz_in_row = (int*)tglib_mmap_wrapper(local_nrow * sizeof(int));
	(*A)->ptr_to_vals_in_row = (double**)tglib_mmap_wrapper(local_nrow * sizeof(double*));
	(*A)->ptr_to_inds_in_row = (long long**)tglib_mmap_wrapper(local_nrow * sizeof(long long*));
	(*A)->ptr_to_diags = (double**)tglib_mmap_wrapper(local_nrow * sizeof(double*));

	*x = (double*)tglib_mmap_wrapper(local_nrow * sizeof(double));
	*b = (double*)tglib_mmap_wrapper(local_nrow * sizeof(double));
	*xexact = (double*)tglib_mmap_wrapper(local_nrow * sizeof(double));
	// Allocate arrays that are of length local_nnz
	(*A)->list_of_vals = (double*)tglib_mmap_wrapper(local_nnz * sizeof(double));
	(*A)->list_of_inds = (long long*)tglib_mmap_wrapper(local_nnz * sizeof(long long));

	double *curvalptr = (*A)->list_of_vals;
	long long *curindptr = (*A)->list_of_inds;

	unsigned long long bz = nx * ny;
	unsigned long long by = nx;

	long long nnzglobal = 0;
	long long max_i = nz * ny * nx;
	long long total_size = nz * ny * nx;
	std::cerr << "Process " << rank << " generating matrix with "
			<< max_i << " rows. Tasksize: " << taskSize << std::endl;
	for (long long start = 0; start < max_i; start += taskSize) {
		//#pragma oss task taskgroup(tg_manager.get_row_taskgroup(start))  label("Generate_Matrix_Task")
		// serialize tasks for correct global variables (not only nzzglobal, which could be a reduce put curindptr and curvalptr in task privates)
		#pragma oss task taskgroup(tg_manager.get_spmv_taskgroup(start)) label("Generate_Matrix_Task") inout(nnzglobal, curvalptr, curindptr)
		{
		std::atomic_thread_fence(std::memory_order_seq_cst);
		int ret = -1;
		//print_mempolicy("BEFORE task set_mempolicy", NULL, 0);
		ret = set_mempolicy(MPOL_LOCAL, NULL, 0);
		if (ret != 0) {
			cerr << "set_mempolicy failed!" << endl;
			abort();
		}
		//print_mempolicy("AFTER task set_mempolicy", NULL, 0);
		const int cpu = sched_getcpu();
		//if (cpu >= 0) {
		//	std::cerr << "[Rank " << rank << "] Generate_Matrix_Task on CPU " << cpu
		//			  << " (rows " << start << " .. " << (std::min(start + taskSize, max_i) - 1) << ")\n";
		//} else {
		//	std::cerr << "[Rank " << rank << "] Generate_Matrix_Task CPU query failed\n";
		//}

		for (long long curlocalrow = start; curlocalrow < std::min(start + taskSize, max_i); curlocalrow++) {
			long long iz = curlocalrow / bz;
			long long iy = ((curlocalrow - iz*bz) / by);
			long long ix = curlocalrow % nx;


			if (debug)
				std::cout << "NEW: " << curlocalrow << " " << iz << " " << iy << " " << ix << " " << curlocalrow << std::endl;

			debug_map[curlocalrow].visited++;
			if (debug_map[curlocalrow].flat != curlocalrow || debug_map[curlocalrow].ix != ix ||
				debug_map[curlocalrow].iy != iy || debug_map[curlocalrow].iz != iz || debug_map[curlocalrow].order != curlocalrow) {
				cerr << "Error in debug map at local row " << curlocalrow << endl;
				cerr << "debug_map[" << curlocalrow << "].flat=" << debug_map[curlocalrow].flat << "|" << curlocalrow << endl;
				cerr << "debug_map[" << curlocalrow << "].ix=" << debug_map[curlocalrow].ix << "|" << ix << endl;
				cerr << "debug_map[" << curlocalrow << "].iy=" << debug_map[curlocalrow].iy << "|" << iy << endl;
				cerr << "debug_map[" << curlocalrow << "].iz=" << debug_map[curlocalrow].iz << "|" << iz << endl;
				cerr << "debug_map[" << curlocalrow << "].order=" << debug_map[curlocalrow].order << "|" << curlocalrow << endl;
				fflush(stderr);
				abort();
			}

			long long currow = start_row + iz * nx * ny + iy * nx + ix;
			int nnzrow = 0;
			(*A)->ptr_to_vals_in_row[curlocalrow] = curvalptr;
			(*A)->ptr_to_inds_in_row[curlocalrow] = curindptr;
			for (int sz = -1; sz <= 1; sz++) {
				for (int sy = -1; sy <= 1; sy++) {
					for (int sx = -1; sx <= 1; sx++) {
						long long curcol = currow + sz * nx * ny + sy * nx + sx;
//            Since we have a stack of nx by ny by nz domains , stacking in the z direction, we check to see
//            if sx and sy are reaching outside of the domain, while the check for the curcol being valid
//            is sufficient to check the z values
						if ((ix + sx >= 0) && (ix + sx < nx) && (iy + sy >= 0) && (iy + sy < ny) && (curcol >= 0 && curcol < total_nrow)) {
							if (!use_7pt_stencil || (sz * sz + sy * sy + sx * sx <= 1)) { // This logic will skip over point that are not part of a 7-pt stencil
								if (curcol == currow) {
									(*A)->ptr_to_diags[curlocalrow] = curvalptr;
									*curvalptr++ = 27.0;
								} else {
									*curvalptr++ = -1.0;
								}
								*curindptr++ = curcol;
								nnzrow++;
							}
						}
					}       // end sx loop
				}               // end sy loop
			}                       // end sz loop
			(*A)->nnz_in_row[curlocalrow] = nnzrow;
			nnzglobal += nnzrow;
			(*x)[curlocalrow] = 0.0;
			(*b)[curlocalrow] = 27.0 - ((double)(nnzrow - 1));
			(*xexact)[curlocalrow] = 1.0;
		}
		std::atomic_thread_fence(std::memory_order_seq_cst);
		}
	}
	#pragma oss taskwait
	if (rank == 0 )
		std::cerr << "Process " << rank << " finished generating " << nnzglobal
				<< " nonzeros." << std::endl;
	if (debug) cout << "Process " << rank << " of " << size << " has " << local_nrow;

	{
		const long page_size = sysconf(_SC_PAGESIZE);
		const int numa_domains = numa_max_node() + 1;

		auto print_buf_numa = [&](const char *name, void *base, size_t bytes) {
			if (base == nullptr || bytes == 0) return;

			const size_t npages = (bytes + (size_t)page_size - 1) / (size_t)page_size;
			void **pages = (void **)malloc(npages * sizeof(void *));
			int *status = (int *)malloc(npages * sizeof(int));
			size_t *counts = (size_t *)calloc((size_t)numa_domains, sizeof(size_t));
			if (!pages || !status || !counts) {
				cerr << "[Rank " << rank << "] NUMA report allocation failed for buffer " << name << endl;
				free(pages);
				free(status);
				free(counts);
				return;
			}

			char *p = (char *)base;
			for (size_t i = 0; i < npages; ++i) pages[i] = (void *)(p + i * (size_t)page_size);

			long rc = move_pages(0, npages, pages, NULL, status, 0);
			if (rc < 0) {
				cerr << "[Rank " << rank << "] move_pages failed for buffer " << name
					 << " (errno=" << errno << ")" << endl;
				free(pages);
				free(status);
				free(counts);
				return;
			}

			size_t unknown = 0;
			for (size_t i = 0; i < npages; ++i) {
				if (status[i] >= 0 && status[i] < numa_domains) counts[status[i]]++;
				else unknown++;
			}

			const double mib = (double)bytes / (1024.0 * 1024.0);
			cerr << "[Rank " << rank << "] Buffer " << name
				 << ": size=" << bytes << " B (" << mib << " MiB), pages=" << npages << endl;

			for (int n = 0; n < numa_domains; ++n) {
				const double pct = npages ? (100.0 * (double)counts[n] / (double)npages) : 0.0;
				cerr << "  NUMA " << n << ": " << counts[n] << " pages (" << pct << "%)" << endl;
			}
			if (unknown) {
				const double pct = npages ? (100.0 * (double)unknown / (double)npages) : 0.0;
				cerr << "  NUMA unknown: " << unknown << " pages (" << pct << "%)" << endl;
			}

			free(pages);
			free(status);
			free(counts);
		};

		size_t total_bytes = 0;
		auto report = [&](const char *name, void *ptr, size_t bytes) {
			total_bytes += bytes;
			print_buf_numa(name, ptr, bytes);
		};

		report("A.nnz_in_row", (void *)(*A)->nnz_in_row, (size_t)local_nrow * sizeof(int));
		report("A.ptr_to_vals_in_row", (void *)(*A)->ptr_to_vals_in_row, (size_t)local_nrow * sizeof(double *));
		report("A.ptr_to_inds_in_row", (void *)(*A)->ptr_to_inds_in_row, (size_t)local_nrow * sizeof(long long *));
		report("A.ptr_to_diags", (void *)(*A)->ptr_to_diags, (size_t)local_nrow * sizeof(double *));
		report("x", (void *)(*x), (size_t)local_nrow * sizeof(double));
		report("b", (void *)(*b), (size_t)local_nrow * sizeof(double));
		report("xexact", (void *)(*xexact), (size_t)local_nrow * sizeof(double));
		report("A.list_of_vals", (void *)(*A)->list_of_vals, (size_t)local_nnz * sizeof(double));
		report("A.list_of_inds", (void *)(*A)->list_of_inds, (size_t)local_nnz * sizeof(long long));
		if (debug_map != nullptr) report("debug_map", (void *)debug_map, (size_t)total_size * sizeof(struct idx));

		cerr << "[Rank " << rank << "] Total tracked buffer size: " << total_bytes
			 << " B (" << ((double)total_bytes / (1024.0 * 1024.0 * 1024.0)) << " GiB)" << endl;
	}

	long long max_rows = nz * ny * nx;
	for (long long i = 0; i < max_rows; i++) {
		if (debug_map[i].visited != 1 || debug_map[i].order != i) {
			cerr << "Error: debug_map[" << i << "] visited " << debug_map[i].visited << " times (expected 1)" << endl;
			fflush(stderr);
			abort();
		}
	}

	if (debug) cout << " rows. Global rows " << start_row
			<< " through " << stop_row << endl;

	if (debug) cout << "Process " << rank << " of " << size
			<< " has " << local_nnz << " nonzeros." << endl;

	(*A)->start_row = start_row;
	(*A)->stop_row = stop_row;
	(*A)->total_nrow = total_nrow;
	(*A)->total_nnz = total_nnz;
	(*A)->local_nrow = local_nrow;
	(*A)->local_ncol = local_nrow;
	(*A)->local_nnz = local_nnz;

	return;
}

void delete_matrix(HPC_Sparse_Matrix **A, double **x, double **b, double **xexact, int nx, int ny, int nz)
{
	long long local_nrow = nx * ny * nz;                          // This is the size of our subblock
	assert(local_nrow > 0);                                 // Must have something to work with
	long long local_nnz = 27 * local_nrow;                        // Approximately 27 nonzeros per row (except for boundary nodes)
    long long *list_of_inds = (*A)->list_of_inds;
    double *list_of_vals = (*A)->list_of_vals;
    double **ptr_to_diags = (*A)->ptr_to_diags;
    long long **ptr_to_inds_in_row = (*A)->ptr_to_inds_in_row;
    double **ptr_to_vals_in_row = (*A)->ptr_to_vals_in_row;
    int *nnz_in_row = (*A)->nnz_in_row;
	tglib_free_mmap_info();
    delete *A;
}
