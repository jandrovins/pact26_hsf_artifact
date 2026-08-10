#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce nosvorig HPCCG results (orig + init)
#
# Results go to $REPO_ROOT/reproduced_results/fox_hpccg_orig/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# Map login /home paths to the /nfs/home mirror the compute nodes see (e.g. Fox);
# no-op where login and compute paths already match (portable on other clusters).
if [ "${REPO_ROOT#/home/}" != "$REPO_ROOT" ] && [ -d "/nfs${REPO_ROOT}" ]; then
	nfsmap(){ printf '/nfs%s' "$1"; }
else
	nfsmap(){ printf '%s' "$1"; }
fi
NFS_REPO="$(nfsmap "$REPO_ROOT")"
export REPRO_BASE_ROOT="$(nfsmap "$(dirname "$SCRIPT_DIR")")"
readonly BIN="HPCCG_mpi_oss-notampi.bin"
readonly NREPS="${NREPS:-3}"
readonly PARTITION="${PARTITION:-fox}"

# Configs: "NX:NY:NZ:NTASKS_LIST:MAXIT:CPUS"
readonly EXPERIMENTS=(
    "288:192:768:96,1536,4608:100:96"
    "384:384:1536:192,3072,9216:40:192"
)

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:30:00"
}

run_variant() {
    local variant=$1
    local numa_interleaved=$2

    if [ "$variant" = "init" ]; then
        local results_subdir="results_final_3bs_init_tasks"
    else
        local results_subdir="results_final_3bs"
    fi

    for exp in "${EXPERIMENTS[@]}"; do
        IFS=':' read -r nx ny nz ntasks_str maxit cpus <<< "$exp"
        IFS=',' read -ra ntasks_arr <<< "$ntasks_str"

        for ntasks in "${ntasks_arr[@]}"; do
            export VVV_TG_ENABLED=0
            export VVV_LOWER_LVL=node
            export VVV_UPPER_LVL=node
            export VVV_AFF_FLEXIBLE=0
            export VVV_MMAP_ENABLED=1
            export VVV_NUMA_INTERLEAVED=$numa_interleaved
            export VVV_BS_FACTOR=4
            export NOSV_CONFIG=nosv.toml
            export NOSV_CONFIG_OVERRIDE="topology.binding=inherit,monitoring.enabled=false,monitoring.verbose=false,hwcounters.backend=none,scheduler.immediate_successor=true"
            export SLURM_NTASKS_PER_NODE=1
            export SLURM_NPROCS=1
            export SRUN_CPUS_PER_TASK=$cpus

            export VVV_EXP_STR="nx${nx}_ny${ny}_nz${nz}_nt${ntasks}_maxit${maxit}_cpus${cpus}_numa${numa_interleaved}"

            local raw_dir="${REPO_ROOT}/reproduced_results/fox_hpccg_orig/${results_subdir}/raw/${VVV_EXP_STR}"
            mkdir -p "$raw_dir"

            cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${VVV_EXP_STR}",
    "nx": $nx, "ny": $ny, "nz": $nz, "ntasks": $ntasks,
    "maxit": $maxit, "tgenabled": 0,
    "lower": "node", "upper": "node", "flex": 0,
    "imm": "true", "ppn": 1, "cpuspertask": $cpus,
    "threads": $cpus, "bsf": 4, "mmap": 1,
    "numainterleaved": $numa_interleaved
}
METAEOF
            local nfs_raw="$(nfsmap "$raw_dir")"
            echo "  [$variant] nx=$nx ny=$ny nz=$nz ntasks=$ntasks cpus=$cpus numa=$numa_interleaved"
            sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
                --switches=1 --export=ALL \
                -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
                --output="${nfs_raw}/job%A_%a.out" \
                --error="${nfs_raw}/job%A_%a.err" \
                "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$nx" "$ny" "$nz" "$maxit" "$ntasks"
        done
    done
}

main() {
    setup_slurm_env
    echo "=== HPCCG nosvorig: orig ==="
    run_variant "orig" 0
    echo "=== HPCCG nosvorig: init (NUMA interleaved) ==="
    run_variant "init" 1
    echo "All jobs submitted."
}

main
