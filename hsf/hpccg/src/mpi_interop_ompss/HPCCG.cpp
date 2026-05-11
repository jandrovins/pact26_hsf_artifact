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
//   contributors may be used end endorse or promote products derived start
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

// Routine end compute an approximate solution end Ax = b where:

// A - known matrix stored as an HPC_Sparse_Matrix struct

// b - known right hand side vector

// x - On entry is initial guess, on exit new approximate solution

// max_iter - Maximum number of iterations end perform, even if
//            tolerance is not met.

// tolerance - Stop and assert convergence if norm of residual is <=
//             end tolerance.

// niters - On output, the number of iterations actually performed.

/////////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cassert>
#include <pthread.h>
#include <vector>
using std::cout;
using std::cerr;
using std::endl;
#include <cmath>
#include "mytimer.hpp"
#include "HPCCG.hpp"
#include "constants.h"
#include <cstdlib>
#include <set>
#include <stdint.h>
#include <unistd.h>

#include <climits>

#include <tglib.h>
#include <linux/mempolicy.h>
#include <numaif.h>

#ifdef USING_OSS
#include <nodes/debug.h>
#include <nosv.h>
#include <nosv/hwinfo.h>
#include <nosv/affinity.h>
#include "TaskGroupManager.hpp"
#else
#include "omp.h"
#endif

#ifdef USING_INTEROP
#include <TAMPI.h>
#elif defined(USING_MPI)
#include <mpi.h>
#endif

#define USING_OSS

//#include <hpcbench.h>

#ifdef USING_OSS
	#pragma message ("Using OmpSs-2 (defined USING_INTEROP and USING_OSS)")
#elif defined(USING_OMP)
	#pragma message ("Using OpenMP (defined USING_INTEROP and USING_OMP)")
#else
	#error ERROR: Must define either USING_OSS or USING_OMP
#endif

#ifdef USING_INTEROP
	#pragma message ("Using Interop (defined USING_INTEROP)")
#elif defined(USING_MPI)
	#pragma message ("USING_INTEROP not defined but defined USING_MPI. This is valid")
#endif

int global_verbose;
pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

#define VERBOSE_MSG_LVL(lvl, format, ...) \
	if (global_verbose == lvl) { \
		pthread_mutex_lock(&print_mutex); \
		fprintf(stderr, "[%d] ", getpid()); \
		for (int i = 0; i < 0; i++) \
			fprintf(stderr, "\t"); \
		/*fprintf(stderr, "%s:%-30s:%-3d VERBOSE:", __FILE__, __func__, __LINE__);*/ \
		fprintf(stderr, "%-30s:%-3d VERBOSE:", __func__, __LINE__); \
		fprintf(stderr, " " format, ##__VA_ARGS__); \
		fprintf(stderr, "\n"); \
		pthread_mutex_unlock(&print_mutex); \
	}

extern int NUM_TASKS;
int TS;


void _waxpby_range(const double alpha, const double *const x, const double beta, const double *const y, double *const w, 
				   const int start, const int end, const int BS_FACTOR = 1, const int priority = 0)
{
	VERBOSE_MSG_LVL(1, "start: %d end: %d x: %p y: %p w: %p", start, end, x, y, w);

	int size = end - start;
	#ifdef USING_OSS
	#pragma oss task priority(priority) in({x[start+TS*f;size], f=0;BS_FACTOR}, {y[start+TS*f;size], f=0;BS_FACTOR}) out({w[start+TS*f;size], f=0;BS_FACTOR}) taskgroup(tg_manager.get_row_taskgroup(start)) label("_waxpby_range")
	#elif defined (USING_OMP)
	#pragma omp task depend(iterator(f=0:BS_FACTOR), in: x[start+TS*f:size], y[start+TS*f:size]) \
					depend(iterator(f=0:BS_FACTOR), out: w[start+TS*f:size]) label("_waxpby_range")
	#endif
	for (int i = start; i < end; i++) w[i] = x[i] + beta * y[i];
}
void _waxpby_range_beta(const double alpha, const double *const x, const double *const beta, const double *const y, double *const w, 
						const int start, const int end, const int BS_FACTOR = 1, const int priority = 0)
{
	VERBOSE_MSG_LVL(1, "start: %d end: %d x: %p beta: %p y: %p w: %p", start, end, x, beta, y, w);

	int size = end - start;
	#ifdef USING_OSS
	#pragma oss task priority(priority) taskgroup(tg_manager.get_row_taskgroup(start)) in({x[start+TS*f;size], f=0;BS_FACTOR}, {y[start+TS*f;size], f=0;BS_FACTOR}, *beta) out({w[start+TS*f;size], f=0;BS_FACTOR}) label("_waxpby_range_beta")
	#elif defined (USING_OMP)
	#pragma omp task depend(iterator(f=0:BS_FACTOR), in: x[start+TS*f:size], y[start+TS*f:size], *beta) \
					depend(iterator(f=0:BS_FACTOR), out: w[start+TS*f:size]) label("_waxpby_range_beta")
	#endif
	for (int i = start; i < end; i++) w[i] = x[i] + *beta * y[i];
}
void _waxpby_range_negative_beta(const double alpha, const double *const x, const double *const beta, const double *const y, double *const w, 
								 const int start, const int end, const int BS_FACTOR = 1, const int priority = 0)
{
	VERBOSE_MSG_LVL(1, "start: %d end: %d x: %p beta: %p y: %p w: %p", start, end, x, beta, y, w);

	int size = end - start;
	#ifdef USING_OSS
	#pragma oss task priority(priority) taskgroup(tg_manager.get_row_taskgroup(start)) in({x[start+TS*f;size], f=0;BS_FACTOR}, {y[start+TS*f;size], f=0;BS_FACTOR}, *beta) out({w[start+TS*f;size], f=0;BS_FACTOR}) label("_waxpby_range_negative_beta")
	#elif defined (USING_OMP)
	#pragma omp task depend(iterator(f=0:BS_FACTOR), in: x[start+TS*f:size], y[start+TS*f:size], *beta) \
					depend(iterator(f=0:BS_FACTOR), out: w[start+TS*f:size]) label("_waxpby_range_negative_beta")
	#endif
	for (int i = start; i < end; i++) w[i] = x[i] + -(*beta) * y[i];
}

void _ddot_range_xx(const double *const x, const double *const y, double &result, const int start, const int end, const int BS_FACTOR = 1, const int priority = 0)
{
	VERBOSE_MSG_LVL(1, "start: %d end: %d x: %p result: %p", start, end, x, &result);

	int size = end - start;
#ifdef USING_OSS // Use OmpSs reduction over tasks
	#pragma oss task priority(priority) taskgroup(tg_manager.get_row_taskgroup(start)) in({x[start+TS*f;size], f=0;BS_FACTOR}) reduction(+:result) label("_ddot_range_xx")
	for (int i = start; i < end; i++) {
		result += x[i] * x[i];
	}
#elif defined(USING_OMP) // Openmp does not support reductions in tasks, do it manually
	#pragma omp task depend(iterator(f=0:BS_FACTOR), in: x[start+TS*f:size]) label("_ddot_range_xx")
	{
	double omppriv_result = 0.0;
	for (int i = start; i < end; i++)
		omppriv_result += x[i] * x[i];
	
	#pragma omp atomic relaxed
	result += omppriv_result;
	}
#endif
}

void _ddot_range_xy(const double *const x, const double *const y, double &result, const int start, const int end, const int BS_FACTOR = 1, const int priority = 0)
{
	VERBOSE_MSG_LVL(1, "start: %d end: %d x: %p y: %p result: %p", start, end, x, y, &result);

	int size = end - start;
#ifdef USING_OSS
	#pragma oss task priority(priority) taskgroup(tg_manager.get_row_taskgroup(start)) in({x[start+TS*f;size], f=0;BS_FACTOR}, {y[start+TS*f;size], f=0;BS_FACTOR}) reduction(+:result) label("_ddot_range_xy")
	for (int i = start; i < end; i++) result += x[i] * y[i];
#elif defined (USING_OMP)
	#pragma omp task depend(iterator(f=0:BS_FACTOR), in: x[start+TS*f:size], y[start+TS*f:size]) label("_ddot_range_xy")
{
	double omppriv_result = 0.0;
	for (int i = start; i < end; i++)
		omppriv_result += x[i] * y[i];
	
	#pragma omp atomic relaxed
	result += omppriv_result;
}
#endif
}

void _HPC_sparsemv_range_tf(Blk_info *blk_info_array, int **const dep_ptr_to_idx_list, const int dep_size, const int block_id,
							const HPC_Sparse_Matrix *const A, const double *const x, double *const y, const int start, const int end, int iter, int max_iter, const int priority = 0)
{
	VERBOSE_MSG_LVL(1, "start: %d end: %d block_id: %d x: %p y: %p", start, end, block_id, x, y);

	const int nrow = (const int)A->local_nrow;

	int size = end - start;
#ifdef USING_OSS
	#pragma oss task taskgroup(tg_manager.get_spmv_taskgroup(start)) in({ \
		x[blk_info_array[dep_ptr_to_idx_list[block_id][d]].start:blk_info_array[dep_ptr_to_idx_list[block_id][d]].end-1], d=0 ; dep_size}) \
						out( y[start;size] ) label("HPC_sparsemv_range")
#elif defined (USING_OMP)
	#pragma omp task depend(iterator(d=0:dep_size), \
					in: x[blk_info_array[dep_ptr_to_idx_list[block_id][d]].start:blk_info_array[dep_ptr_to_idx_list[block_id][d]].end-1]) \
					depend(out: y[start:size]) label("HPC_sparsemv_range")
#endif
	for (int i = start; i < end; i++) {
		double sum = 0.0;
		const double *const cur_vals =
			(const double *const)A->ptr_to_vals_in_row[i];

		const long long *const cur_inds =
			(const long long *const)A->ptr_to_inds_in_row[i];

		const int cur_nnz = (const int)A->nnz_in_row[i];

		for (int j = 0; j < cur_nnz; j++)
			sum += cur_vals[j] * x[cur_inds[j]];
		y[i] = sum;
	}
}

int HPCCG(HPC_Sparse_Matrix *A,
	  const double *const b, double *const x,
	  const int max_iter, const double tolerance, int &niters, double &normr,
	  double *times, unsigned long int &total_mem)
{
	pthread_mutex_init(&print_mutex, NULL);

	global_verbose = 0;
	const char* vvv_env = getenv("VVV_LVL");
	if (vvv_env != NULL) {
		global_verbose = atoi(vvv_env);
	}

	int start, end;
	int start1, end1;

	double t0 = 0.0, t1 = 0.0, t2 = 0.0, t3 = 0.0, t4 = 0.0;

	double t5 = 0.0;

	int rank; // Number of MPI processes, My process ID
#ifdef USING_MPI
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm MPI_COMM_WORLD2, MPI_COMM_WORLD3;
	MPI_Comm_dup(MPI_COMM_WORLD, &MPI_COMM_WORLD2);
	MPI_Comm_dup(MPI_COMM_WORLD, &MPI_COMM_WORLD3);
#else
	rank = 0;
#endif

	int nrow = A->local_nrow;
	int ncol = A->local_ncol;

	double *r = (double *)tglib_mmap_wrapper(nrow * sizeof(double));
	double *p = (double *)tglib_mmap_wrapper(ncol * sizeof(double));
	double *Ap = (double *)tglib_mmap_wrapper(nrow * sizeof(double));

#ifdef USING_OSS
	int cpus = nanos6_get_num_cpus();
#else
		int cpus = 1;
#pragma omp parallel
        {
		#pragma omp master
		cpus = omp_get_num_threads();
        }
#endif


	int chunk, num_blocks, extra_chunk, extra_blocks;

    int taskSize = std::ceil((double) nrow/(double) NUM_TASKS);
	if (rank == 0)
		std::cerr << "taskSize=" << taskSize << std::endl;
    TS = taskSize;
	assert(taskSize <= nrow);
	chunk = taskSize;
    num_blocks = std::ceil((double) nrow/(double) taskSize);
	extra_chunk = ncol - nrow;
#ifdef USING_MPI
	extra_blocks =  A->num_send_neighbors;
#else
	extra_blocks = 0;
#endif

	Blk_info *blk_info_array = new Blk_info [num_blocks + extra_blocks];
	for (int g = 0; g < num_blocks; ++g) {
		blk_info_array[g].start = g * chunk;
		blk_info_array[g].end = (g + 1) * chunk;
		if (g == num_blocks - 1) {
			blk_info_array[g].end = nrow;
		}
	}
	for (int g = 0; g < extra_blocks; ++g) {
#ifdef USING_MPI
		blk_info_array[num_blocks + g].start = nrow + g * A->recv_length[g];
		blk_info_array[num_blocks + g].end = nrow + (g + 1) * A->recv_length[g];
#else
		assert(0);
#endif
		if (g == extra_blocks - 1) {
			blk_info_array[num_blocks + g].end = ncol;
		}
	}

	// NUMA-aware first-touch initialization of CG working vectors.
	// Each task runs on the correct taskgroup, calling set_mempolicy(MPOL_LOCAL)
	// to place pages on the local NUMA node matching the task's affinity.
#ifdef USING_OSS
	for (int g = 0; g < num_blocks; g++) {
		int ft_start = blk_info_array[g].start;
		int ft_end = blk_info_array[g].end;
		#pragma oss task taskgroup(tg_manager.get_row_taskgroup(ft_start)) \
			out(r[ft_start;ft_end-ft_start], Ap[ft_start;ft_end-ft_start], p[ft_start;ft_end-ft_start]) \
			label("init_cg_vectors")
		{
			//set_mempolicy(MPOL_LOCAL, NULL, 0);
			for (int i = ft_start; i < ft_end; i++) {
				r[i] = 0.0; Ap[i] = 0.0; p[i] = 0.0;
			}
		}
	}
	// Also first-touch the extra (MPI halo) region of p
	if (extra_blocks > 0) {
		for (int g = 0; g < extra_blocks; g++) {
			int ft_start = blk_info_array[num_blocks + g].start;
			int ft_end = blk_info_array[num_blocks + g].end;
			#pragma oss task out(p[ft_start;ft_end-ft_start]) label("init_cg_halo")
			{
				//set_mempolicy(MPOL_LOCAL, NULL, 0);
				for (int i = ft_start; i < ft_end; i++) {
					p[i] = 0.0;
				}
			}
		}
	}
	#pragma oss taskwait
#endif

	bool *deps = new bool [ncol];
	int *dep_idx_list = new int [27 * (num_blocks + extra_blocks)];
	int **dep_ptr_to_idx_list = new int* [num_blocks];
	int *dep_size = new int [num_blocks];

	std::fill(deps, deps + ncol, false);
	std::fill(dep_size, dep_size + num_blocks, 0);
	std::fill(dep_idx_list, dep_idx_list + 27 * (num_blocks + extra_blocks), -1);


	total_mem  = (sizeof(double)*nrow)*2
		+ sizeof(double)*ncol
		+ sizeof(Blk_info)*(num_blocks)
		+ sizeof(bool)*(ncol)
		+ sizeof(int)*27*(num_blocks + extra_blocks)
		+ sizeof(int *)*(num_blocks)
		+ sizeof(int)*(num_blocks);

	if (rank == 0) {
		std::cout << "11. r --- Sizeof double*nrow: " << sizeof(double)*nrow << std::endl;
		std::cout << "12. p --- Sizeof double*ncol: " << sizeof(double)*ncol << std::endl;
		std::cout << "13. Ap --- Sizeof double*nrow: " << sizeof(double)*nrow << std::endl;
		std::cout << "14. blk_info_array --- Sizeof Blk_info*num_blocks: " << sizeof(Blk_info)*(num_blocks) << std::endl;
		std::cout << "15. deps --- Sizeof bool*ncol: " << sizeof(bool)*(ncol) << std::endl;
		std::cout << "16. dep_idx_list --- Sizeof int*27*(num_blocks): " << sizeof(int)*27*(num_blocks) << std::endl;
		std::cout << "17. dep_ptr_to_idx_list --- Sizeof (int *)*num_blocks: " << sizeof(int *)*(num_blocks) << std::endl;
		std::cout << "18. dep_size --- Sizeof int*num_blocks: " << sizeof(int)*(num_blocks) << std::endl;


		std::cout << "Total memory allocated: " << total_mem << std::endl;
		std::cout << "--------------- ALLOCATIONS ---------------" << std::endl;


		std::cout << "NUM_PART: " << NUM_PART
			<< " || nrow: " << nrow
			<< " || cpus: " << cpus
			<< " || chunk: " << chunk
			<< " || num_blocks: " << num_blocks
			<< std::endl;
	}

	int *cur_idx = dep_idx_list;
	for (int g = 0; g < num_blocks; g++) {
		start = blk_info_array[g].start;
		end = blk_info_array[g].end;
		for (int i = start; i < end; i++) {
			const double *const cur_vals =
				(const double *const)A->ptr_to_vals_in_row[i];

			const long long *const cur_inds =
				(const long long *const)A->ptr_to_inds_in_row[i];

			const int cur_nnz = (const int)A->nnz_in_row[i];

			for (int j = 0; j < cur_nnz; j++)
				deps[cur_inds[j]] = true;
		}
		dep_ptr_to_idx_list[g] = cur_idx;
		for (int g1 = 0; g1 < num_blocks + extra_blocks; ++g1) {
			start1 = blk_info_array[g1].start;
			end1 = blk_info_array[g1].end;
			for (int i = start1; i < end1; i++) {
				if (deps[i] && dep_ptr_to_idx_list[g] == cur_idx) {
					*cur_idx++ = g1;
					++dep_size[g];
				}
				else if (deps[i] && cur_idx[-1] != g1) {
					*cur_idx++ = g1;
					++dep_size[g];
				}
			}
		}
		std::fill(deps, deps + ncol, false);
	}

	delete [] deps;

	// Compute dependences indexes for exchange_externals Fill send_buffer task.
	std::vector<int> fsb_dep_indexes;
#ifdef USING_MPI
	for(int i = 0; i < A->total_to_be_sent; i++) {
		int block_start = (A->elements_to_send[i]/taskSize)*taskSize;
		assert(block_start % taskSize == 0);
		if (fsb_dep_indexes.empty()) {
			fsb_dep_indexes.push_back(block_start);
		}
		else {
			bool found = false;
			for(int j = 0; j < fsb_dep_indexes.size(); j++) { 
				if (fsb_dep_indexes[j] == block_start) {
					found = true;
					break;
				}
			}
			if (!found) {
				fsb_dep_indexes.push_back(block_start);
			}
		}
	}
#endif

	double rtrans = 0.0;
	double rtrans_local = 0.0;
	double oldrtrans = 0.0;

	double alpha_local = 0.0;
	double alpha = 0.0;

	double beta = 0.0;

	int print_freq = max_iter / 10;
	if (print_freq > 50) print_freq = 50;
	if (print_freq < 1) print_freq = 1;

#ifdef USING_MPI
	MPI_Barrier(MPI_COMM_WORLD);
#endif

	int ret;
	//char *appidstring = getenv("HPCBENCH_APPID");
	//if (appidstring != NULL) {
	//	int appid = atoi(appidstring);
	//	ret = hpcbench_initialize(appid, "hpccg");
	//	assert(!ret);
	//}

	double t_begin = mytimer(); // Start timing right away

	#ifdef USING_OMP
	#pragma omp parallel
	{
	#pragma omp single nowait
	{
	#endif

	const int k0_prio = max_iter * 1000;

	// p is of length ncols, copy x end p for sparse MV operation
	int BS_FACTOR = 4;
	{
		const char *bs_env = getenv("VVV_BS_FACTOR");
		if (bs_env != NULL) {
			BS_FACTOR = atoi(bs_env);
			if (BS_FACTOR < 1) BS_FACTOR = 1;
			if (BS_FACTOR > num_blocks) BS_FACTOR = num_blocks;
		}
	}
	if (rank == 0) std::cerr << "BS_FACTOR=" << BS_FACTOR << std::endl;
	for (int g = 0; g < num_blocks; g+=BS_FACTOR) {
		start = blk_info_array[g].start;
        int g_end = g+BS_FACTOR-1 < num_blocks ? g+BS_FACTOR-1 : num_blocks-1;
		end = blk_info_array[g_end].end;

		_waxpby_range(1.0, x, 0.0, x, p, start, end, BS_FACTOR, k0_prio + 10);
	}

#ifdef USING_MPI
	exchange_externals(A, p, fsb_dep_indexes);
#endif

	for (int g = 0; g < num_blocks; g+=BS_FACTOR) {
		start = blk_info_array[g].start;
        int g_end = g+BS_FACTOR-1 < num_blocks ? g+BS_FACTOR-1 : num_blocks-1;
		end = blk_info_array[g_end].end;

        for (int g2 = g; g2 <= g_end; g2++) {
            int start2 = blk_info_array[g2].start;
            int end2 = blk_info_array[g2].end;
            _HPC_sparsemv_range_tf(blk_info_array, dep_ptr_to_idx_list, dep_size[g2], g2, A, p, Ap, start2, end2, 0, max_iter, 0);
        }
		_waxpby_range(1.0, b, -1.0, Ap, r, start, end, BS_FACTOR, k0_prio + 9);
		_ddot_range_xx(r, r, rtrans_local, start, end, BS_FACTOR, k0_prio + 8);
	}

#ifdef USING_OSS
	#pragma oss task inout(rtrans_local) out(rtrans) label("MPI_Allreduce_0")
#elif defined(USING_OMP)
	#pragma omp task depend(inout: rtrans_local) depend(out: rtrans) label("MPI_Allreduce_0")
#endif
	{
#ifdef USING_MPI
		#ifdef USING_INTEROP
		TAMPI_Iallreduce(&rtrans_local, &rtrans,
					1,
					MPI_DOUBLE, MPI_SUM,
					MPI_COMM_WORLD);
		#else
		MPI_Allreduce(&rtrans_local, &rtrans,
					1,
					MPI_DOUBLE, MPI_SUM,
					MPI_COMM_WORLD);
		#endif
#else
		rtrans = rtrans_local;
#endif
		rtrans_local = 0.0;
	}

#ifdef USING_OSS
	#pragma oss task in(rtrans) out(normr) label("compute_normr_0")
#elif defined(USING_OMP)
	#pragma omp task depend(in: rtrans) depend(out: normr) label("compute_normr_0")
#endif
	{
		normr = sqrt(rtrans);
		if (rank == 0) cout << "Initial Residual = " << normr << endl;
	}

	int k;
	for (k = 1; k < max_iter; k++) {
		const int iter_prio = (max_iter - k) * 1000;
		if (k == 1) {
            for (int g = 0; g < num_blocks; g+=BS_FACTOR) {
                start = blk_info_array[g].start;
                int g_end = g+BS_FACTOR-1 < num_blocks ? g+BS_FACTOR-1 : num_blocks-1;
                end = blk_info_array[g_end].end;

				_waxpby_range(1.0, r, 0.0, r, p, start, end, BS_FACTOR, iter_prio+10);
			}
		} else {
            for (int g = 0; g < num_blocks; g+=BS_FACTOR) {
                start = blk_info_array[g].start;
                int g_end = g+BS_FACTOR-1 < num_blocks ? g+BS_FACTOR-1 : num_blocks-1;
                end = blk_info_array[g_end].end;

				_ddot_range_xx(r, r, rtrans_local, start, end, BS_FACTOR, iter_prio + 10);
			}

		#ifdef USING_OSS
			#pragma oss task inout(rtrans_local, rtrans) out(oldrtrans) label("MPI_Allreduce_1")
		#elif defined(USING_OMP)
			#pragma omp task depend(inout: rtrans_local, rtrans) depend(out: oldrtrans) label("MPI_Allreduce_1")
		#endif
			{
				oldrtrans = rtrans;
#ifdef USING_MPI
		#ifdef USING_INTEROP
		TAMPI_Iallreduce(&rtrans_local, &rtrans,
						1,
						MPI_DOUBLE, MPI_SUM,
						MPI_COMM_WORLD2);
		#else
		MPI_Allreduce(&rtrans_local, &rtrans,
						1,
						MPI_DOUBLE, MPI_SUM,
						MPI_COMM_WORLD2);
		#endif
#else
				rtrans = rtrans_local;
#endif
				rtrans_local = 0.0;
			}

		#ifdef USING_OSS
			#pragma oss task in(oldrtrans, rtrans) out(beta) label("compute_beta")
		#elif defined(USING_OMP)
			#pragma omp task depend(in: oldrtrans, rtrans) depend(out: beta) label("compute_beta")
		#endif
			{
				beta = rtrans / oldrtrans;
			}

            for (int g = 0; g < num_blocks; g+=BS_FACTOR) {
                start = blk_info_array[g].start;
                int g_end = g+BS_FACTOR-1 < num_blocks ? g+BS_FACTOR-1 : num_blocks-1;
                end = blk_info_array[g_end].end;

				_waxpby_range_beta(1.0, r, &beta, p, p, start, end, BS_FACTOR, iter_prio + 9);
			}
		}

#ifdef USING_MPI
		exchange_externals(A, p, fsb_dep_indexes);
#endif

        for (int g = 0; g < num_blocks; g+=BS_FACTOR) {
            start = blk_info_array[g].start;
            int g_end = g+BS_FACTOR-1 < num_blocks ? g+BS_FACTOR-1 : num_blocks-1;
            end = blk_info_array[g_end].end;

            for (int g2 = g; g2 <= g_end; g2++) {
                int start2 = blk_info_array[g2].start;
                int end2 = blk_info_array[g2].end;
                _HPC_sparsemv_range_tf(blk_info_array, dep_ptr_to_idx_list, dep_size[g2], g2, A, p, Ap, start2, end2, k, max_iter, iter_prio + 8);
            }
			_ddot_range_xy(p, Ap, alpha_local, start, end, BS_FACTOR, iter_prio + 7);
		}

	#ifdef USING_OSS
		#pragma oss task inout(alpha_local) out(alpha) in(rtrans) priority (iter_prio + 20) label("MPI_Allreduce_2")
	#elif defined(USING_OMP)
		#pragma omp task depend(inout: alpha_local) depend(out: alpha) depend(in: rtrans) label("MPI_Allreduce_2")
	#endif
		{
#ifdef USING_MPI
		#ifdef USING_INTEROP
		TAMPI_Iallreduce(&alpha_local, &alpha,
						1,
						MPI_DOUBLE, MPI_SUM,
						MPI_COMM_WORLD3);
		#else
		MPI_Allreduce(&alpha_local, &alpha,
						1,
						MPI_DOUBLE, MPI_SUM,
						MPI_COMM_WORLD3);
		#endif
#else
			alpha = alpha_local;
#endif
			alpha_local = 0.0;
		}

	#ifdef USING_OSS
		#pragma oss task in(rtrans) out(normr) inout(alpha) in(rtrans) priority(iter_prio + 30) label("compute_normr_alpha")
	#elif defined(USING_OMP)
		#pragma omp task depend(in: rtrans) depend(out: normr) depend(inout: alpha) label("compute_normr_alpha")
	#endif
		{
			normr = sqrt(rtrans);
			alpha = rtrans / alpha;
		}

        for (int g = 0; g < num_blocks; g+=BS_FACTOR) {
            start = blk_info_array[g].start;
            int g_end = g+BS_FACTOR-1 < num_blocks ? g+BS_FACTOR-1 : num_blocks-1;
            end = blk_info_array[g_end].end;

			_waxpby_range_beta(1.0, x, &alpha, p, x, start, end, BS_FACTOR, iter_prio + 5);
			_waxpby_range_negative_beta(1.0, r, &alpha, Ap, r, start, end, BS_FACTOR, iter_prio + 4);
		}
	}

	//#ifdef USING_OMP
	//#pragma omp master
	//#endif
    //double time_end = mytimer() - t_begin;
    //std::cout << "Time spent creating: " << time_end << std::endl;

	#ifdef USING_OSS
	#pragma oss taskwait
	#elif defined(USING_OMP)
	//#pragma omp taskwait
	#endif

	niters = k;

	#ifdef USING_OMP
	} // end single region
	} // end parallel region
	#endif

	times[0] = mytimer() - t_begin; // Total time. All done...

	//if (appidstring != NULL) {
	//	ret = hpcbench_finalize(NULL);
	//	assert(!ret);
	//}

#ifdef USING_MPI
	MPI_Barrier(MPI_COMM_WORLD);
#endif

	// Store times
	times[1] = t1;  // ddot time
	times[2] = t2;  // waxpby time
	times[3] = t3;  // sparsemv time
	times[4] = t4;  // AllReduce time
	times[5] = t5;  // exchange boundary time

	delete [] dep_idx_list;
	delete [] dep_ptr_to_idx_list;
	delete [] dep_size;

	delete [] blk_info_array;

	// r, p, Ap are freed by tglib_free_mmap_info() in delete_matrix()
	return 0;
}
