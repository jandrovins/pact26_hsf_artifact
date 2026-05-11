CC        = clang
GCC       = gcc
CFLAGS    = -O3  -lm -ggdb3 -fno-omit-frame-pointer -fno-omit-frame-pointer
OSS_FLAGS = -fompss-2=libnodes
OMP_FLAGS = -fopenmp

all: overhead_oss overhead_omp overhead_oss_taskloop overhead_omp_taskloop overhead_omp_gcc_taskloop overhead_omp_gcc measure_work

overhead_oss: main.c tasks_oss.c common.h
	$(CC) $(CFLAGS) $(OSS_FLAGS) -o $@ main.c tasks_oss.c

overhead_omp: main.c tasks_omp.c common.h
	$(CC) $(CFLAGS) $(OMP_FLAGS) -o $@ main.c tasks_omp.c

# Parallel task-launching variants
overhead_oss_taskloop: main.c tasks_oss_taskloop.c common.h
	$(CC) $(CFLAGS) $(OSS_FLAGS) -o $@ main.c tasks_oss_taskloop.c

overhead_omp_taskloop: main.c tasks_omp_taskloop.c common.h
	$(CC) $(CFLAGS) $(OMP_FLAGS) -o $@ main.c tasks_omp_taskloop.c

overhead_omp_gcc_taskloop: main.c tasks_omp_taskloop.c common.h
	$(GCC) $(CFLAGS) $(OMP_FLAGS) -o $@ main.c tasks_omp_taskloop.c

overhead_omp_gcc: main.c tasks_omp.c common.h
	$(GCC) $(CFLAGS) $(OMP_FLAGS) -o $@ main.c tasks_omp.c

# Taskgroup variants (3-level and 4-level hierarchies)
overhead_oss_tg: main.c tasks_oss_tg.c topo_tg.c topo_tg.h common.h
	$(CC) $(CFLAGS) $(OSS_FLAGS) -o $@ main.c tasks_oss_tg.c topo_tg.c

overhead_oss_tg_taskloop: main.c tasks_oss_tg_taskloop.c topo_tg.c topo_tg.h common.h
	$(CC) $(CFLAGS) $(OSS_FLAGS) -o $@ main.c tasks_oss_tg_taskloop.c topo_tg.c

overhead_oss_tggrp: main.c tasks_oss_tggrp.c topo_tg.c topo_tg.h common.h
	$(CC) $(CFLAGS) $(OSS_FLAGS) -o $@ main.c tasks_oss_tggrp.c topo_tg.c

overhead_oss_tggrp_taskloop: main.c tasks_oss_tggrp_taskloop.c topo_tg.c topo_tg.h common.h
	$(CC) $(CFLAGS) $(OSS_FLAGS) -o $@ main.c tasks_oss_tggrp_taskloop.c topo_tg.c


measure_work: measure_work.c common.h
	$(CC) $(CFLAGS) -o $@ measure_work.c

clean:
	rm -f overhead_oss overhead_omp overhead_oss_taskloop overhead_omp_taskloop \
		overhead_oss_tg overhead_oss_tg_taskloop overhead_oss_tggrp overhead_oss_tggrp_taskloop \
		overhead_omp_gcc overhead_omp_gcc_taskloop \
		measure_work
