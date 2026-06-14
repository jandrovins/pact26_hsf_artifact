#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include "heat.h"


int main(int argc, char **argv)
{
	HeatConfiguration conf;
	readConfiguration(argc, argv, &conf);
	refineConfiguration(&conf, conf.rbs, conf.cbs);
	if (conf.verbose)
		printConfiguration(&conf);

	int64_t rows = conf.rows+2;
	int64_t cols = conf.cols+2;

	initialize(&conf, rows, cols, 0);

	if (conf.warmup)
		solve(&conf, rows, cols, 1, NULL);

	// Solve the problem
	double start = getTime();
	solve(&conf, rows, cols, conf.timesteps, NULL);
	double residual = 0.0;
	double end = getTime();
	double delta_time = end - start;

	long niter = conf.convergenceTimesteps;
	long iter_elem = conf.rows * conf.cols;
	long total_elem = iter_elem * niter;
	double throughput = total_elem / delta_time;

//	fprintf(stderr, "%14s %14s %14s %8s %8s %8s %8s %8s\n",
//			"time", "updates/s", "rel. error", 
//			"rows", "cols",
//			"rbs", "cbs", "iters");
	fprintf(stdout, "%14e %14e %14e %8ld %8ld %8d %8d %8ld %s\n",
			delta_time, throughput, residual,
			conf.rows, conf.cols, 
			conf.rbs, conf.cbs, niter, BENCH6_NAME);

	if (conf.generateImage)
		writeImage(conf.imageFileName, conf.matrix, rows, cols);

	finalize(&conf);

	return 0;
}
