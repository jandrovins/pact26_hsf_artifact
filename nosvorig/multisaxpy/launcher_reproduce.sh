#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce nosvorig Multisaxpy results (init only)
#
# NOTE: No "orig" (no-init) results exist — baseline was too slow.
# Results go to $REPO_ROOT/reproduced_results/fox_multisaxpy_orig/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

readonly BIN="b6_multisaxpy_prio"
readonly NREPS=3
readonly PARTITION="${PARTITION:-fox}"

# Init configs: "N:TS:ITERATIONS:CPUS:IMM:MMAP:PRIO"
readonly CONFIGS=(
    "56524800:29440:1000:96:true:1:1"
    "56524800:58880:1000:96:false:0:1"
    "56524800:588800:1000:96:true:1:1"
    "771740160:29440:100:192:true:1:1"
    "771740160:58880:100:192:false:0:0"
    "771740160:401948:100:192:true:1:0"
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
    echo "=== Multisaxpy nosvorig: init ==="

    for cfg in "${CONFIGS[@]}"; do
        IFS=':' read -r n ts iterations cpus imm mmap prio <<< "$cfg"

        local topo_bind="inherit"
        if [ "$cpus" = "96" ]; then
            topo_bind="0-95"
        fi

        export VVV_MMAP_ENABLED=$mmap
        export VVV_PRIO_ENABLED=$prio
        export NOSV_CONFIG=nosv.toml
        export NOSV_CONFIG_OVERRIDE="topology.binding=${topo_bind},monitoring.enabled=false,monitoring.verbose=false,hwcounters.backend=none,scheduler.immediate_successor=$imm"
        export SLURM_NTASKS_PER_NODE=1
        export SLURM_NPROCS=1
        export SRUN_CPUS_PER_TASK=$cpus

        export VVV_EXP_STR="N${n}_TS${ts}_its${iterations}_cpus${cpus}_imm${imm}_mmap${mmap}_prio${prio}"

        local raw_dir="${REPO_ROOT}/reproduced_results/fox_multisaxpy_orig/results_final_3bs_init_tasks/raw/${VVV_EXP_STR}"
        mkdir -p "$raw_dir"

        cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${VVV_EXP_STR}",
    "imm": "$imm", "mmap": $mmap, "prio": $prio,
    "N": $n, "TS": $ts, "iterations": $iterations,
    "cpus": $cpus, "ppn": 1
}
METAEOF
        local nfs_raw="$(echo "$raw_dir" | sed 's|^/home/|/nfs/home/|')"

        echo "  [init] N=$n TS=$ts its=$iterations cpus=$cpus imm=$imm prio=$prio"
        sbatch -p "$PARTITION" --array=1-${NREPS} \
            --switches=1 --export=ALL \
            -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
            --output="${nfs_raw}/job%A_%a.out" \
            --error="${nfs_raw}/job%A_%a.err" \
            "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$n" "$ts" "$iterations"
    done
    echo "All jobs submitted."
}

main
