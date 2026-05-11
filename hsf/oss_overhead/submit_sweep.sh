#!/usr/bin/env bash
#SBATCH --account=bsc15
#SBATCH --qos=gp_bsccs
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=112
#SBATCH --exclusive
#SBATCH --hint=nomultithread
#SBATCH --job-name=oss_overhead_sweep
#SBATCH --output=%x_%j.out
#SBATCH --error=%x_%j.err

FLAKE_DIR=/gpfs/projects/bsc15/bsc015889/ics_amdblis
SCRIPT_DIR=/gpfs/projects/bsc15/bsc015889/ics_amdblis/oss_overhead

nix develop --impure "$FLAKE_DIR" --command bash -c "cd '$SCRIPT_DIR' && ./run_sweep.sh"
