#include "common.h"
#include <time.h>

void submit_tasks(task_info_t *info, long ntasks, long niters) {
    for (long i = 0; i < ntasks; i++) {
        #pragma oss task default(none) shared(info) firstprivate(i, niters, ntasks)
        {
            clock_gettime(CLOCK_MONOTONIC, &info[i].start);
            info[i].result = pi_leibniz(niters);
            clock_gettime(CLOCK_MONOTONIC, &info[i].end);
            info[i].id = (int)i;
        }
    }
    #pragma oss taskwait
}
