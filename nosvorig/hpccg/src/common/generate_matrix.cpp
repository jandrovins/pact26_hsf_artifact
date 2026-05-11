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

#include <iostream>
using std::cout;
using std::cerr;
using std::endl;
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <unistd.h>
#include <sched.h>
#include <numa.h>
#include <numaif.h>
#include "valloc.h"
#include "generate_matrix.hpp"

static int hpccg_numa_interleaved = 0;
static struct bitmask *hpccg_nodemask = NULL;

static void* hpccg_alloc(size_t sz)
{
	if (hpccg_numa_interleaved) {
		void *p = numa_alloc_interleaved_subset(sz, hpccg_nodemask);
		if (!p) { perror("numa_alloc_interleaved_subset"); exit(1); }
		return p;
	} else {
		return valloc(sz);
	}
}

static void printNumaDistribution(const char *name, void *ptr, size_t size)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t num_pages = (size + page_size - 1) / page_size;

	void **pages = (void **)malloc(num_pages * sizeof(void *));
	int  *status = (int *)malloc(num_pages * sizeof(int));
	if (!pages || !status) { perror("malloc"); free(pages); free(status); return; }

	for (size_t i = 0; i < num_pages; i++)
		pages[i] = (char *)ptr + i * page_size;

	if (move_pages(0, num_pages, pages, NULL, status, 0) < 0) {
		perror("move_pages"); free(pages); free(status); return;
	}

	int max_node = numa_max_node() + 1;
	long *counts = (long *)calloc(max_node, sizeof(long));
	if (!counts) { free(pages); free(status); return; }

	for (size_t i = 0; i < num_pages; i++)
		if (status[i] >= 0 && status[i] < max_node)
			counts[status[i]]++;

	fprintf(stderr, "NUMA page distribution for %s (%zu pages, page_size=%ld):\n",
		name, num_pages, page_size);
	for (int n = 0; n < max_node; n++) {
		fprintf(stderr, "  NUMA node %d: %ld pages (%.2f%%)  (%ld bytes)\n",
			n, counts[n],
			100.0 * counts[n] / (double)num_pages,
			counts[n] * page_size);
	}

	free(pages); free(status); free(counts);
}
void generate_matrix(int nx, int ny, int nz, HPC_Sparse_Matrix **A, double **x, double **b, double **xexact)

{
#ifdef DEBUG
	int debug = 1;
#else
	int debug = 0;
#endif

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

	long long local_nrow = (long long)nx * (long long)ny * (long long)nz;                          // This is the size of our subblock
	assert(local_nrow > 0);                                 // Must have something to work with
	long long local_nnz = 27 * local_nrow;                        // Approximately 27 nonzeros per row (except for boundary nodes)

	long long total_nrow = local_nrow * size;                     // Total number of grid points in mesh
	long long total_nnz = 27 * (long long)total_nrow;       // Approximately 27 nonzeros per row (except for boundary nodes)

	long long start_row = local_nrow * rank;                      // Each processor gets a section of a chimney stack domain
	long long stop_row = start_row + local_nrow - 1;

	size_t bytes = 0;
	bytes += sizeof(HPC_Sparse_Matrix); // struct itself
	bytes += sizeof(long long) * local_nrow; // nnz_in_row
	bytes += sizeof(double*) * local_nrow; // ptr_to_vals_in_row
	bytes += sizeof(long long*) * local_nrow; // ptr_to_inds_in_row
	bytes += sizeof(double*) * local_nrow; // ptr_to_diags
	bytes += sizeof(double) * local_nrow; // x
	bytes += sizeof(double) * local_nrow; // b
	bytes += sizeof(double) * local_nrow; // xexact
	bytes += sizeof(double) * local_nnz; // list_of_vals
	bytes += sizeof(long long) * local_nnz; // list_of_inds

	double gib = bytes / (1024.0 * 1024.0 * 1024.0);
	cout << "Estimated memory to be allocated: " << gib << " GiB" << endl;


	// Check environment variable for NUMA interleaved allocation
	{
		char *env = getenv("VVV_NUMA_INTERLEAVED");
		hpccg_numa_interleaved = env && atoi(env) != 0;
		if (hpccg_numa_interleaved) {
			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
			hpccg_nodemask = numa_allocate_nodemask();
			int num_cpus = numa_num_possible_cpus();
			for (int cpu = 0; cpu < num_cpus; cpu++) {
				if (CPU_ISSET(cpu, &cpuset)) {
					int node = numa_node_of_cpu(cpu);
					if (node >= 0)
						numa_bitmask_setbit(hpccg_nodemask, node);
				}
			}
			fprintf(stderr, "NUMA interleaved allocation on nodes:");
			int max_node = numa_max_node() + 1;
			for (int n = 0; n < max_node; n++)
				if (numa_bitmask_isbitset(hpccg_nodemask, n))
					fprintf(stderr, " %d", n);
			fprintf(stderr, "\n");
		}
	}

	// Allocate arrays that are of length local_nrow
	if (!hpccg_numa_interleaved) valloc_init();
	(*A)->nnz_in_row = static_cast<int*>(
		hpccg_alloc(sizeof(int) * local_nrow));
	(*A)->ptr_to_vals_in_row = static_cast<double**>(
		hpccg_alloc(sizeof(double*) * local_nrow));
	(*A)->ptr_to_inds_in_row = static_cast<long long**>(
		hpccg_alloc(sizeof(long long*) * local_nrow));
	(*A)->ptr_to_diags = static_cast<double**>(
		hpccg_alloc(sizeof(double*) * local_nrow));

	*x = static_cast<double*>(
		hpccg_alloc(sizeof(double) * local_nrow));
	*b = static_cast<double*>(
		hpccg_alloc(sizeof(double) * local_nrow));
	*xexact = static_cast<double*>(
		hpccg_alloc(sizeof(double) * local_nrow));

	// Allocate arrays that are of length local_nnz
	(*A)->list_of_vals = static_cast<double*>(
		hpccg_alloc(sizeof(double) * local_nnz));
	(*A)->list_of_inds = static_cast<long long*>(
		hpccg_alloc(sizeof(long long) * local_nnz));

	double *curvalptr = (*A)->list_of_vals;
	long long *curindptr = (*A)->list_of_inds;

	long long nnzglobal = 0;
	for (long long iz = 0; iz < nz; iz++) {
		for (long long iy = 0; iy < ny; iy++) {
			for (long long ix = 0; ix < nx; ix++) {
				long long curlocalrow = iz * nx * ny + iy * nx + ix;
				long long currow = start_row + iz * nx * ny + iy * nx + ix;
				long long nnzrow = 0;
				(*A)->ptr_to_vals_in_row[curlocalrow] = curvalptr;
				(*A)->ptr_to_inds_in_row[curlocalrow] = curindptr;
				for (long long sz = -1; sz <= 1; sz++) {
					for (long long sy = -1; sy <= 1; sy++) {
						for (long long sx = -1; sx <= 1; sx++) {
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
			}       // end ix loop
		}               // end iy loop
	}                       // end iz loop

	if (hpccg_numa_interleaved) {
		printNumaDistribution("list_of_vals", (*A)->list_of_vals, sizeof(double) * local_nnz);
		printNumaDistribution("list_of_inds", (*A)->list_of_inds, sizeof(long long) * local_nnz);
		printNumaDistribution("x", *x, sizeof(double) * local_nrow);
		printNumaDistribution("b", *b, sizeof(double) * local_nrow);
		printNumaDistribution("xexact", *xexact, sizeof(double) * local_nrow);
	}

	if (debug) cout << "Process " << rank << " of " << size << " has " << local_nrow;

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
	long long local_nrow = (long long)nx * (long long)ny * (long long)nz;                          // This is the size of our subblock
	assert(local_nrow > 0);                                 // Must have something to work with
	long long local_nnz = 27 * local_nrow;                        // Approximately 27 nonzeros per row (except for boundary nodes)
    long long *list_of_inds = (*A)->list_of_inds;
    double *list_of_vals = (*A)->list_of_vals;
    double **ptr_to_diags = (*A)->ptr_to_diags;
    long long **ptr_to_inds_in_row = (*A)->ptr_to_inds_in_row;
    double **ptr_to_vals_in_row = (*A)->ptr_to_vals_in_row;
    int *nnz_in_row = (*A)->nnz_in_row;
	if (hpccg_nodemask) { numa_free_nodemask(hpccg_nodemask); hpccg_nodemask = NULL; }
	if (hpccg_numa_interleaved) {
		numa_free(nnz_in_row, sizeof(int) * local_nrow);
		numa_free(ptr_to_vals_in_row, sizeof(double*) * local_nrow);
		numa_free(ptr_to_inds_in_row, sizeof(long long*) * local_nrow);
		numa_free(ptr_to_diags, sizeof(double*) * local_nrow);
		numa_free(*x, sizeof(double) * local_nrow);
		numa_free(*b, sizeof(double) * local_nrow);
		numa_free(*xexact, sizeof(double) * local_nrow);
		numa_free(list_of_vals, sizeof(double) * local_nnz);
		numa_free(list_of_inds, sizeof(long long) * local_nnz);
	} else {
		valloc_cleanup();
	}

    delete *A;
}
