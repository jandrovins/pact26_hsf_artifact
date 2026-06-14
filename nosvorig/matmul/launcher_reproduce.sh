#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce nosvorig Matmul results (orig + init)
#
# Results go to $REPO_ROOT/reproduced_results/fox_matmul_orig/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NFS_REPO="$(echo "$REPO_ROOT" | sed 's|^/home/|/nfs/home/|')"

readonly BIN="02.matmul_ompss2_itampi.bin"
readonly NREPS=3
readonly PARTITION="${PARTITION:-fox}"

# Configs: "NSIZE:MSIZE:TS_LIST:ITS:CPUS"
readonly EXPERIMENTS=(
    "6144:6144:192,256,512:20:96"
    "49152:49152:192,256,512:1:192"
)

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:20:00"
}

run_variant() {
    local variant=$1
    local numa=$2

    if [ "$variant" = "init" ]; then
        local results_subdir="results_final_3bs_init_tasks"
    else
        local results_subdir="results_final_3bs"
    fi

    for exp in "${EXPERIMENTS[@]}"; do
        IFS=':' read -r nsize msize ts_str its cpus <<< "$exp"
        IFS=',' read -ra ts_arr <<< "$ts_str"

        for ts in "${ts_arr[@]}"; do
            export VVV_MMAP_ENABLED=1
            export VVV_NUMA_INTERLEAVED=$numa
            export NOSV_CONFIG=nosv.toml
            export NOSV_CONFIG_OVERRIDE="topology.binding=inherit,monitoring.enabled=false,monitoring.verbose=false,hwcounters.backend=none,scheduler.immediate_successor=true"
            export SLURM_NTASKS_PER_NODE=1
            export SLURM_NPROCS=1
            export SRUN_CPUS_PER_TASK=$cpus

            export VVV_EXP_STR="n${nsize}_m${msize}_ts${ts}_its${its}_cpus${cpus}_numa${numa}"

            local raw_dir="${REPO_ROOT}/reproduced_results/fox_matmul_orig/${results_subdir}/raw/${VVV_EXP_STR}"
            mkdir -p "$raw_dir"

            cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${VVV_EXP_STR}",
    "imm": "true", "ppn": 1, "cpuspertask": $cpus,
    "nsize": $nsize, "msize": $msize, "ts": $ts,
    "its": $its, "mmap": 1, "numa": $numa
}
METAEOF
            local nfs_raw="$(echo "$raw_dir" | sed 's|^/home/|/nfs/home/|')"

            echo "  [$variant] N=$nsize TS=$ts its=$its cpus=$cpus numa=$numa"
            sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
                --switches=1 --export=ALL \
                -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
                --output="${nfs_raw}/job%A_%a.out" \
                --error="${nfs_raw}/job%A_%a.err" \
                "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$nsize" "$msize" "$its" "$ts"
        done
    done
}

main() {
    setup_slurm_env
    echo "=== Matmul nosvorig: orig ==="
    run_variant "orig" 0
    echo "=== Matmul nosvorig: init (NUMA) ==="
    run_variant "init" 1
    echo "All jobs submitted."
}

main
