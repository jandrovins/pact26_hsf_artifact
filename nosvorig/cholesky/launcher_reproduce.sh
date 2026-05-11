#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce nosvorig Cholesky results (orig + init)
#
# Submits SLURM jobs for 6 configs x 2 variants (orig, init) x 3 reps = 36 jobs
# Results go to $REPO_ROOT/reproduced_results/fox_cholesky_orig/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NFS_REPO="$(echo "$REPO_ROOT" | sed 's|^/home/|/nfs/home/|')"

readonly BIN="cholesky_oss.bin"
readonly NREPS=3
readonly PARTITION="${PARTITION:-fox}"

# Configurations from original results:
# Small: N=6144, TS=128,192,256, cpus=96
# Large: N=33792, TS=256,512,1024, cpus=192
readonly EXPERIMENTS=(
    "6144:128,192,256:96"
    "33792:256,512,1024:192"
)

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:10:00"
}

run_variant() {
    local variant=$1  # "orig" or "init"
    local numa_interleaved=$2

    if [ "$variant" = "init" ]; then
        local results_subdir="results_final_3bs_init_tasks"
    else
        local results_subdir="results_final_3bs"
    fi

    for exp in "${EXPERIMENTS[@]}"; do
        IFS=':' read -r nsize ts_str cpus <<< "$exp"
        IFS=',' read -ra ts_arr <<< "$ts_str"

        for ts in "${ts_arr[@]}"; do
            export VVV_EXP_STR="N${nsize}_TS${ts}_imm_true_mmap1"
            export VVV_MMAP_ENABLED=1
            export VVV_TG_ENABLED=0
            export VVV_NUMA_INTERLEAVED=$numa_interleaved
            export NOSV_CONFIG=nosv.toml
            export NOSV_CONFIG_OVERRIDE="topology.binding=inherit,monitoring.enabled=false,monitoring.verbose=false,hwcounters.backend=none,scheduler.immediate_successor=true"

            local raw_dir="${REPO_ROOT}/reproduced_results/fox_cholesky_orig/${results_subdir}/raw/${VVV_EXP_STR}"
            mkdir -p "$raw_dir"

            # Write metadata for collect_results.py
            cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${VVV_EXP_STR}",
    "N": $nsize, "TS": $ts,
    "lower": "node", "upper": "node", "affflex": 0,
    "useprio": 0, "gemmtpb": 48, "tgenabled": 0,
    "imm": "true", "ppn": 1, "mmap": 1
}
METAEOF
            local nfs_raw="$(echo "$raw_dir" | sed 's|^/home/|/nfs/home/|')"

            echo "  [$variant] N=$nsize TS=$ts cpus=$cpus numa=$numa_interleaved"
            sbatch -p "$PARTITION" --array=1-${NREPS} \
                --switches=1 --export=ALL \
                -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
                --output="${nfs_raw}/job%A_%a.out" \
                --error="${nfs_raw}/job%A_%a.err" \
                "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$nsize" "$ts"
        done
    done
}

main() {
    setup_slurm_env
    echo "=== Cholesky nosvorig: orig (no NUMA init) ==="
    run_variant "orig" 0
    echo "=== Cholesky nosvorig: init (NUMA interleaved) ==="
    run_variant "init" 1
    echo "All jobs submitted."
}

main
