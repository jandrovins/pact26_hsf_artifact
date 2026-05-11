#include "common.h"
#include <time.h>
#include <unistd.h>
#include <nosv/hwinfo.h>

/* Parallel task launching: the outer taskloop creates one "creator task"
 * per core (grainsize = ntasks/ncores).  Each creator runs on a separate
 * worker thread and submits its slice of ntasks leaf tasks, so task
 * creation is spread across all cores. */
void submit_tasks(task_info_t *info, long ntasks, long niters) {
    long ncores = nosv_get_num_domains(NOSV_TOPO_LEVEL_CORE);
    long tl_ntasks = ncores;
    long grain  = (ntasks + tl_ntasks - 1) / tl_ntasks;

    #pragma oss taskloop grainsize(grain) shared(info) firstprivate(niters)
    for (long i = 0; i < ntasks; i++) {
        #pragma oss task shared(info) firstprivate(i, niters)
        {
            clock_gettime(CLOCK_MONOTONIC, &info[i].start);
            info[i].result = pi_leibniz(niters);
            clock_gettime(CLOCK_MONOTONIC, &info[i].end);
            info[i].id = (int)i;
        }
    }
    /* Wait for all creator tasks and their leaf-task descendants. */
    #pragma oss taskwait
}
