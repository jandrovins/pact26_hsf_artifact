#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce HSF Multisaxpy results (tg variant)
#
# Results go to $REPO_ROOT/reproduced_results/fox_multisaxpy_tg/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NFS_REPO="$(echo "$REPO_ROOT" | sed 's|^/home/|/nfs/home/|')"

readonly BIN="b6_multisaxpy_nodes"
readonly NREPS=3
readonly PARTITION="${PARTITION:-fox}"

# HSF configs: "N:TS:ITERATIONS:CPUS:LOWER:POLICY:LABEL"
readonly CONFIGS=(
    "56524800:29440:1000:96:core:PRIO:prio_per_core"
    "56524800:58880:1000:96:core:PRIO:prio_per_core"
    "56524800:588800:1000:96:cs:PRIO:prio_per_cs"
    "771740160:29440:100:192:core:PRIO:prio_per_core"
    "771740160:58880:100:192:core:PRIO:prio_per_core"
    "771740160:401948:100:192:core:PRIO:prio_per_core"
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
    echo "=== Multisaxpy HSF (taskgroups) ==="

    for cfg in "${CONFIGS[@]}"; do
        IFS=':' read -r n ts iterations cpus lower policy label <<< "$cfg"

        local topo_bind="inherit"
        if [ "$cpus" = "96" ]; then
            topo_bind="0-95"
        fi

        export VVV_TG_ENABLED=1
        export VVV_LOWER_LVL=$lower
        export VVV_TG_POLICY=$policy
        export VVV_MMAP_ENABLED=1
        export NOSV_CONFIG=nosv.toml
        export NOSV_CONFIG_OVERRIDE="topology.binding=${topo_bind},hwcounters.backend=none,scheduler.immediate_successor=false,taskgroups.save_hierarchy=false"
        export SLURM_NTASKS_PER_NODE=1
        export SLURM_NPROCS=1
        export SRUN_CPUS_PER_TASK=$cpus

        export VVV_EXP_STR="N${n}_TS${ts}_its${iterations}_cpus${cpus}_lower${lower}_policy${policy}"

        local raw_dir="${REPO_ROOT}/reproduced_results/fox_multisaxpy_tg/results_final_3bs/raw/${VVV_EXP_STR}"
        mkdir -p "$raw_dir"

        cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${VVV_EXP_STR}",
    "label": "$label", "lower": "$lower", "policy": "$policy",
    "N": $n, "TS": $ts, "iterations": $iterations,
    "cpus": $cpus, "ppn": 1
}
METAEOF
        local nfs_raw="$(echo "$raw_dir" | sed 's|^/home/|/nfs/home/|')"

        echo "  [tg] N=$n TS=$ts lower=$lower policy=$policy"
        sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
            --switches=1 --export=ALL \
            -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
            --output="${nfs_raw}/job%A_%a.out" \
            --error="${nfs_raw}/job%A_%a.err" \
            "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$n" "$ts" "$iterations"
    done
    echo "All jobs submitted."
}

main
