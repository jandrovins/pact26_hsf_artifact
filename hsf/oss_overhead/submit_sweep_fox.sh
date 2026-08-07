#!/usr/bin/env bash
#SBATCH --partition=fox
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=192
#SBATCH --exclusive
#SBATCH --job-name=oss_overhead_sweep
#SBATCH --output=%x_%j.out
#SBATCH --error=%x_%j.err

SCRIPT_DIR="${SLURM_SUBMIT_DIR:-$(cd "$(dirname "$0")" && pwd)}"
FLAKE_DIR="$(dirname "$SCRIPT_DIR")"

nix develop --impure "$FLAKE_DIR" --command bash -c "cd '$SCRIPT_DIR' && ./run_sweep.sh"
