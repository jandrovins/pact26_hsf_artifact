#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce external baseline Multisaxpy (OpenMP)
#
# Pure OpenMP repeated SAXPY (no OmpSs-2 / nOS-V runtime).
# Configs taken from results/fox_multisaxpy_omp/summary.csv.
# Results -> $REPO_ROOT/reproduced_results/fox_multisaxpy_omp/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NFS_REPO="$(echo "$REPO_ROOT" | sed 's|^/home/|/nfs/home/|')"

readonly BIN="multisaxpy_smp"
readonly NREPS=3
readonly PARTITION="${PARTITION:-fox}"
readonly RESULT_DIR="${REPO_ROOT}/reproduced_results/fox_multisaxpy_omp"

# "N:ITS:CPUS"  (each run for numa in {0,1} x procbind in {0,1})
readonly EXPERIMENTS=(
    "56524800:100:96"
    "771740160:1000:192"
)
readonly NUMAS=(0 1)
readonly PROCBINDS=(0 1)

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:30:00"
}

main() {
    setup_slurm_env
    echo "=== Multisaxpy baseline (OpenMP) ==="

    for exp in "${EXPERIMENTS[@]}"; do
        IFS=':' read -r n its cpus <<< "$exp"
        for numa in "${NUMAS[@]}"; do
            for procbind in "${PROCBINDS[@]}"; do
                export VVV_NUMA_INTERLEAVED=$numa
                export OMP_PROC_BIND=$procbind

                local exp_str="n${n}_iters${its}_numa${numa}_procbind${procbind}"
                local raw_dir="${RESULT_DIR}/raw/${exp_str}"
                mkdir -p "$raw_dir"

                cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${exp_str}",
    "ppn": 1, "cpuspertask": $cpus, "nsize": $n, "iters": $its,
    "numa": $numa, "procbind": $procbind
}
METAEOF
                local nfs_raw="$(echo "$raw_dir" | sed 's|^/home/|/nfs/home/|')"

                echo "  [multisaxpy] n=$n its=$its numa=$numa procbind=$procbind"
                sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
                    --switches=1 --export=ALL \
                    -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
                    --output="${nfs_raw}/job%A_%a.out" \
                    --error="${nfs_raw}/job%A_%a.err" \
                    "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$n" "$its" "$cpus" "$numa" "$procbind"
            done
        done
    done
    echo "All jobs submitted."
}

main
