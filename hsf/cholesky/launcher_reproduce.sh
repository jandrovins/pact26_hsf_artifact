#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce HSF Cholesky results (tg variant)
#
# Submits SLURM jobs for 6 configs x 3 reps = 18 jobs
# Results go to $REPO_ROOT/reproduced_results/fox_cholesky_tg/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NFS_REPO="$(echo "$REPO_ROOT" | sed 's|^/home/|/nfs/home/|')"

readonly BIN="cholesky_oss.bin"
readonly NREPS=3
readonly PARTITION="${PARTITION:-fox}"

# HSF configs from original results:
# Small: N=6144, TS=128,192,256, lower=cs, upper=node, affflex=1
# Large: N=33792, TS=256,512,1024, lower=numa, upper=numa, affflex=0
readonly SMALL_CONFIGS=(
    "6144:128:cs:node:1:96"
    "6144:192:cs:node:1:96"
    "6144:256:cs:node:1:96"
)
readonly LARGE_CONFIGS=(
    "33792:256:numa:numa:0:192"
    "33792:512:numa:numa:0:192"
    "33792:1024:numa:numa:0:192"
)

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:10:00"
}

run_config() {
    local nsize=$1 ts=$2 lower=$3 upper=$4 affflex=$5 cpus=$6

    export VVV_TG_ENABLED=1
    export VVV_LOWER_LVL=$lower
    export VVV_UPPER_LVL=$upper
    export VVV_AFF_FLEXIBLE=$affflex
    export VVV_MMAP_ENABLED=1
    export VVV_PRIORITY_ENABLED=1
    export VVV_CHOL_GEMM_TILES_PER_BLOCK=48
    export NOSV_CONFIG=nosv.toml
    export NOSV_CONFIG_OVERRIDE="topology.binding=inherit,hwcounters.backend=none,scheduler.immediate_successor=false,taskgroups.save_hierarchy=false"

    export VVV_EXP_STR="N${nsize}_TS${ts}_lower${lower}_upper${upper}_flex${affflex}"

    local raw_dir="${REPO_ROOT}/reproduced_results/fox_cholesky_tg/results_final_3bs/raw/${VVV_EXP_STR}"
    mkdir -p "$raw_dir"

    cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${VVV_EXP_STR}",
    "N": $nsize, "TS": $ts,
    "lower": "$lower", "upper": "$upper", "affflex": $affflex,
    "useprio": 1, "gemmtpb": 48, "tgenabled": 1,
    "imm": "false", "ppn": 1, "mmap": 1
}
METAEOF
    local nfs_raw="$(echo "$raw_dir" | sed 's|^/home/|/nfs/home/|')"

    echo "  [tg] N=$nsize TS=$ts lower=$lower upper=$upper flex=$affflex"
    sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
        --switches=1 --export=ALL \
        -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
        --output="${nfs_raw}/job%A_%a.out" \
        --error="${nfs_raw}/job%A_%a.err" \
        "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$nsize" "$ts"
}

main() {
    setup_slurm_env
    echo "=== Cholesky HSF (taskgroups) ==="
    for cfg in "${SMALL_CONFIGS[@]}" "${LARGE_CONFIGS[@]}"; do
        IFS=':' read -r nsize ts lower upper affflex cpus <<< "$cfg"
        run_config "$nsize" "$ts" "$lower" "$upper" "$affflex" "$cpus"
    done
    echo "All jobs submitted."
}

main
