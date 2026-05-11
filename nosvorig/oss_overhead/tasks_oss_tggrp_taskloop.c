#include "common.h"
#include "topo_tg.h"
#include <time.h>

#ifndef TOPO_MAX_LEVEL
#define TOPO_MAX_LEVEL TOPO_TG_LEVEL_CORE
#endif

void submit_tasks(task_info_t *info, long ntasks, long niters) {
	topo_tg_grp_init_level(TOPO_MAX_LEVEL);
	int *prios = topo_tg_get_prios(ntasks);
	long tl_ntasks = topo_tg_num_cores()*16;
	long per = (ntasks + tl_ntasks - 1) / tl_ntasks;
	#pragma oss taskloop label("taskloop") grainsize(per) shared(info, prios) firstprivate(niters, ntasks)
	for (long i = 0; i < ntasks; i++) {
		nosv_task_group_t tg = topo_tg_grp_get_level(i, ntasks);
		int prio = prios ? prios[i] : 0;
		#pragma oss task label("pi_leibniz") taskgroup(tg) priority(prio) \
			shared(info) firstprivate(i, niters, tg, prio)
		{
			clock_gettime(CLOCK_MONOTONIC, &info[i].start);
			info[i].result = pi_leibniz(niters);
			clock_gettime(CLOCK_MONOTONIC, &info[i].end);
			info[i].id = (int)i;
		}
	}
	#pragma oss taskwait
}
