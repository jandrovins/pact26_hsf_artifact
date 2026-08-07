#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce external baseline Matmul (mt-dgemm, OpenMP+BLIS)
#
# Pure OpenMP-threaded cblas_dgemm via AOCL BLIS (no OmpSs-2 / nOS-V runtime).
# Configs taken from results/fox_mt-dgemm_libomp/summary.csv.
# Results -> $REPO_ROOT/reproduced_results/fox_mt-dgemm_libomp/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NFS_REPO="$REPO_ROOT"
export REPRO_BASE_ROOT="$(dirname "$SCRIPT_DIR")"
readonly BIN="mt-dgemm"
readonly NREPS=3
readonly PARTITION="${PARTITION:-fox}"
readonly RESULT_DIR="${REPO_ROOT}/reproduced_results/fox_mt-dgemm_libomp"

# "N:ITS:CPUS"  (each run for numa in {0,1})
readonly SIZES=(
    "6144:100:96"
    "49152:1:192"
)
readonly NUMAS=(0 1)

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:30:00"
}

main() {
    setup_slurm_env
    echo "=== Matmul baseline (mt-dgemm OpenMP+BLIS) ==="

    for sz in "${SIZES[@]}"; do
        IFS=':' read -r n its cpus <<< "$sz"
        for numa in "${NUMAS[@]}"; do
            export VVV_NUMA_INTERLEAVED=$numa

            local exp_str="n${n}_its${its}_numa${numa}"
            local raw_dir="${RESULT_DIR}/raw/${exp_str}"
            mkdir -p "$raw_dir"

            cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${exp_str}",
    "ppn": 1, "cpuspertask": $cpus, "nsize": $n, "msize": $n,
    "its": $its, "numa": $numa
}
METAEOF
            local nfs_raw="$raw_dir"
            echo "  [matmul] n=$n its=$its cpus=$cpus numa=$numa"
            sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
                --switches=1 --export=ALL \
                -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
                --output="${nfs_raw}/job%A_%a.out" \
                --error="${nfs_raw}/job%A_%a.err" \
                "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$n" "$its" "$cpus" "$numa"
        done
    done
    echo "All jobs submitted."
}

main
