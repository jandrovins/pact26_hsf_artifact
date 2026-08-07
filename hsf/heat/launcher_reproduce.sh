#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce HSF Heat results (tg variant)
#
# Results go to $REPO_ROOT/reproduced_results/fox_heat_tg/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NFS_REPO="$REPO_ROOT"
export REPRO_BASE_ROOT="$(dirname "$SCRIPT_DIR")"
readonly BIN="02.heat_ompss2.bin"
readonly NREPS="${NREPS:-3}"
readonly PARTITION="${PARTITION:-fox}"
readonly WARMUP=1

# HSF configs: "N:BS:ITS:CPUS:LOWER:UPPER:FLEX"
# Small (N=12288): strict CCD affinity
# Large (N=49152): flexible CCD->NUMA affinity
readonly CONFIGS=(
    "12288:256:800:96:cs:cs:0"
    "12288:768:800:96:cs:cs:0"
    "12288:1024:800:96:cs:cs:0"
    "49152:256:60:192:cs:numa:1"
    "49152:512:60:192:cs:numa:1"
    "49152:2048:60:192:cs:numa:1"
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
    echo "=== Heat HSF (taskgroups) ==="

    for cfg in "${CONFIGS[@]}"; do
        IFS=':' read -r n bs its cpus lower upper flex <<< "$cfg"

        local topo_bind="inherit"
        if [ "$cpus" = "96" ]; then
            topo_bind="0-95"
        fi

        export VVV_TG_ENABLED=1
        export VVV_LOWER_LVL=$lower
        export VVV_UPPER_LVL=$upper
        export VVV_AFF_FLEXIBLE=$flex
        export VVV_TG_POLICY=PRIO
        export VVV_DIAGS=1
        export VVV_PRIORITY=0
        export VVV_BLOCKFUNC=0
        export NOSV_CONFIG=nosv.toml
        export NOSV_CONFIG_OVERRIDE="topology.binding=${topo_bind},hwcounters.backend=none,scheduler.immediate_successor=false,taskgroups.save_hierarchy=false"

        export VVV_EXP_STR="n${n}_bs${bs}_its${its}_lower${lower}_upper${upper}_flex${flex}"

        local raw_dir="${REPO_ROOT}/reproduced_results/fox_heat_tg/results_final_3bs/raw/${VVV_EXP_STR}"
        mkdir -p "$raw_dir"

        cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${VVV_EXP_STR}",
    "n": $n, "bs": $bs, "its": $its,
    "diags": 1, "prio": 0, "bf": 0, "tge": 1,
    "lower": "$lower", "upper": "$upper", "flex": $flex, "policy": "PRIO"
}
METAEOF
        local nfs_raw="$raw_dir"
        echo "  [tg] n=$n bs=$bs lower=$lower upper=$upper flex=$flex"
        sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
            --switches=1 --export=ALL \
            -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
            --output="${nfs_raw}/job%A_%a.out" \
            --error="${nfs_raw}/job%A_%a.err" \
            "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$n" "$bs" "$its" "$WARMUP" "$cpus"
    done
    echo "All jobs submitted."
}

main
