#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce HSF Matmul results (tg variant)
#
# Results go to $REPO_ROOT/reproduced_results/fox_matmul_tg/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

readonly BIN="02.matmul_ompss2_itampi.bin"
readonly NREPS=3
readonly PARTITION="${PARTITION:-fox}"

# HSF configs: "N:M:TS:ITS:CPUS:LOWER:UPPER:AFFFLEX"
# Small (N=6144): strict CCD affinity
# Large (N=49152): per-TS affinity tuning
readonly CONFIGS=(
    "6144:6144:192:20:96:cs:cs:0"
    "6144:6144:256:20:96:cs:cs:0"
    "6144:6144:512:20:96:cs:cs:0"
    "49152:49152:192:1:192:cs:numa:1"
    "49152:49152:256:1:192:numa:numa:0"
    "49152:49152:512:1:192:numa:node:1"
)

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:20:00"
}

main() {
    setup_slurm_env
    echo "=== Matmul HSF (taskgroups) ==="

    for cfg in "${CONFIGS[@]}"; do
        IFS=':' read -r n m ts its cpus lower upper affflex <<< "$cfg"

        export VVV_TG_ENABLED=1
        export VVV_LOWER_LVL=$lower
        export VVV_UPPER_LVL=$upper
        export VVV_AFF_FLEXIBLE=$affflex
        export VVV_MMAP_ENABLED=1
        export VVV_FORCE_NBLOCKS=1
        export VVV_L3_SIZE_MIB=96
        export VVV_MATMUL_TG_HIERARCHY=0
        export NOSV_CONFIG=nosv.toml
        export NOSV_CONFIG_OVERRIDE="topology.binding=inherit,hwcounters.backend=none,scheduler.immediate_successor=false,taskgroups.save_hierarchy=false"
        export SLURM_NTASKS_PER_NODE=1
        export SLURM_NPROCS=1
        export SRUN_CPUS_PER_TASK=$cpus

        export VVV_EXP_STR="N${n}_M${m}_TS${ts}_its${its}_lower${lower}_upper${upper}_flex${affflex}"

        local raw_dir="${REPO_ROOT}/reproduced_results/fox_matmul_tg/results_final_3bs/raw/${VVV_EXP_STR}"
        mkdir -p "$raw_dir"

        cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${VVV_EXP_STR}",
    "N": $n, "M": $m, "TS": $ts,
    "tgenabled": 1, "hier": 0,
    "lower": "$lower", "upper": "$upper", "affflex": $affflex,
    "imm": "false", "ppn": 1, "mmap": 1,
    "forcenb": 1, "hwc": "none", "l3": 96,
    "its": $its
}
METAEOF
        local nfs_raw="$(echo "$raw_dir" | sed 's|^/home/|/nfs/home/|')"

        echo "  [tg] N=$n TS=$ts lower=$lower upper=$upper flex=$affflex"
        sbatch -p "$PARTITION" --array=1-${NREPS} \
            --switches=1 --export=ALL \
            -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
            --output="${nfs_raw}/job%A_%a.out" \
            --error="${nfs_raw}/job%A_%a.err" \
            "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$n" "$m" "$its" "$ts"
    done
    echo "All jobs submitted."
}

main
