#include "common.h"
#include <time.h>
#include <omp.h>

void submit_tasks(task_info_t *info, long ntasks, long niters) {
    #pragma omp parallel default(none) shared(info) firstprivate(ntasks, niters)
    {
        #pragma omp master
        {
            long nthreads = omp_get_num_threads();
            long per = (ntasks + nthreads - 1) / nthreads;
            #pragma omp taskloop grainsize(per)
            for (long i = 0; i < ntasks; i++) {
                #pragma omp task firstprivate(i)
                {
                    clock_gettime(CLOCK_MONOTONIC, &info[i].start);
                    info[i].result = pi_leibniz(niters);
                    clock_gettime(CLOCK_MONOTONIC, &info[i].end);
                    info[i].id = (int)i;
                }
            }
            /* taskloop's implicit taskgroup ensures all leaf tasks
             * are done before we leave this block. */
        }
    }
}
