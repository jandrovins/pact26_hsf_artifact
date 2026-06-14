#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce external baseline HPCCG (MPI + OpenMP)
#
# Pure MPI+OpenMP conjugate-gradient solver (no OmpSs-2 / nOS-V runtime).
# Configs taken from results/fox_hpccg_omp/summary.csv (numa=1 only).
# Results -> $REPO_ROOT/reproduced_results/fox_hpccg_omp/
#
# nx ny nz are GLOBAL dims; the binary takes LOCAL dims, so nz_local = nz/ppn
# (ppn ranks stacked along z). cpus-per-task = total_cores / ppn.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NFS_REPO="$(echo "$REPO_ROOT" | sed 's|^/home/|/nfs/home/|')"

readonly BIN="test_HPCCG"
readonly NREPS=3
readonly PARTITION="${PARTITION:-fox}"
readonly RESULT_DIR="${REPO_ROOT}/reproduced_results/fox_hpccg_omp"
readonly NUMA=1

# "nx ny nz maxit total_cores ppns_csv"
readonly CONFIGS=(
    "288 192 768 100 96 1,4"
    "384 384 1536 40 192 1,8"
)

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:30:00"
}

main() {
    setup_slurm_env
    echo "=== HPCCG baseline (MPI+OpenMP) ==="

    for cfg in "${CONFIGS[@]}"; do
        read -r nx ny nz maxit total ppns_csv <<< "$cfg"
        IFS=',' read -ra ppns <<< "$ppns_csv"
        for ppn in "${ppns[@]}"; do
            local cpus=$(( total / ppn ))
            local nzlocal=$(( nz / ppn ))
            export VVV_NUMA_INTERLEAVED=$NUMA
            export OMP_PROC_BIND=true

            local exp_str="ppn${ppn}_nx${nx}_ny${ny}_nz${nz}_maxit${maxit}_nzlocal${nzlocal}_cpuspertask${cpus}_numa${NUMA}"
            local raw_dir="${RESULT_DIR}/raw/${exp_str}"
            mkdir -p "$raw_dir"

            cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${exp_str}",
    "ppn": $ppn, "cpuspertask": $cpus,
    "nx": $nx, "ny": $ny, "nz": $nz, "maxit": $maxit,
    "nzlocal": $nzlocal, "numa": $NUMA
}
METAEOF
            local nfs_raw="$(echo "$raw_dir" | sed 's|^/home/|/nfs/home/|')"

            echo "  [hpccg] ppn=$ppn nx=$nx ny=$ny nz=$nz (nzlocal=$nzlocal) maxit=$maxit cpus=$cpus"
            sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
                --switches=1 --export=ALL \
                -N 1 --ntasks-per-node=$ppn --cpus-per-task=$cpus \
                --output="${nfs_raw}/job%A_%a.out" \
                --error="${nfs_raw}/job%A_%a.err" \
                "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$nx" "$ny" "$nzlocal" "$maxit" "$ppn" "$cpus" "$NUMA"
        done
    done
    echo "All jobs submitted."
}

main
