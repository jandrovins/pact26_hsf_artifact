#ifndef HEAT_H
#define HEAT_H

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

#define IGNORE_RESIDUAL (NAN)
#define DEFAULT_DELTA ((double) 0.05)
#define DEFAULT_RELAX 1.9766
#define DEFAULT_BS 1024

#define ROUND(a, b) ((((a) + (b) - 1) / (b)) * (b))

typedef struct {
	float row;
	float col;
	float range;
	float temperature;
} HeatSource;

typedef struct {
	int timesteps;
	int convergenceTimesteps;
	double delta;
	double relax;
	int64_t rows;
	int64_t cols;
	int rbs;
	int cbs;
	int nwins;
	double *matrix;
	int numHeatSources;
	HeatSource *heatSources;
	char confFileName[PATH_MAX];
	char imageFileName[PATH_MAX];
	bool generateImage;
	bool warmup;
	bool verbose;
} HeatConfiguration;


void initialize(HeatConfiguration *conf, int64_t rows, int64_t cols, int64_t rowOffset);
void finalize(HeatConfiguration *conf);
void printNumaDistribution(const char *name, void *ptr, size_t size);
void writeImage(const char *fileName, double *matrix, int64_t rows, int64_t cols);
void readConfiguration(int argc, char **argv, HeatConfiguration *conf);
void refineConfiguration(HeatConfiguration *conf, int64_t rowValue, int64_t colValue);
void printConfiguration(const HeatConfiguration *conf);
void initializeMatrix(const HeatConfiguration *conf, double *matrix, int64_t rows, int64_t cols, int64_t rowOffset);
double getTime(void);

const char *summary(void);
double solve(HeatConfiguration *conf, int64_t rows, int64_t cols, int timesteps, void *extraData);
void computeBlock(const int64_t rows, const int64_t cols, const int rstart, const int rend, const int cstart, const int cend, double M[rows][cols]);
void computeBlockResidual(const int64_t rows, const int64_t cols, const int rstart, const int rend, const int cstart, const int cend, double M[rows][cols], double relax, double *residual, double *max_elem);

#endif // HEAT_H
