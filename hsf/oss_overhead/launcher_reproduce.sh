#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce the OmpSs-2 task-overhead microbenchmark sweep
#
# Builds the overhead_* binaries and runs run_sweep.sh (NRUNS=5) inside the
# parent flake's dev-shell. Fully relocatable: all paths derive from this
# script's location. Results go to this directory's results/ subdir.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# Map login /home paths to the /nfs/home mirror the compute nodes see (e.g. Fox);
# no-op where login and compute paths already match (portable on other clusters).
if [ "${REPO_ROOT#/home/}" != "$REPO_ROOT" ] && [ -d "/nfs${REPO_ROOT}" ]; then
	nfsmap(){ printf '/nfs%s' "$1"; }
else
	nfsmap(){ printf '%s' "$1"; }
fi
NFS_REPO="$(nfsmap "$REPO_ROOT")"
NFS_SCRIPT="$(nfsmap "$SCRIPT_DIR")"
export REPRO_BASE_ROOT="$(nfsmap "$(dirname "$SCRIPT_DIR")")"   # parent flake dir (nosvorig | hsf)
readonly PARTITION="${PARTITION:-fox}"
readonly VARIANT="$(basename "$REPRO_BASE_ROOT")"

mkdir -p "$SCRIPT_DIR/results"

echo "=== oss_overhead sweep: ${VARIANT} (partition=${PARTITION}) ==="
sbatch -p "$PARTITION" --chdir="$NFS_REPO" --export=ALL \
    -N 1 --ntasks-per-node=1 --cpus-per-task=192 --exclusive \
    --hint=nomultithread \
    --job-name="oss_overhead_${VARIANT}" \
    --output="${NFS_SCRIPT}/oss_overhead_${VARIANT}_%j.out" \
    --error="${NFS_SCRIPT}/oss_overhead_${VARIANT}_%j.err" \
    "$SCRIPT_DIR/submit_reproduce.job"
echo "Job submitted."
