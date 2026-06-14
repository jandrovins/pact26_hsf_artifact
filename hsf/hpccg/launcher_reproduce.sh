#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce HSF HPCCG results (tg variant)
#
# Results go to $REPO_ROOT/reproduced_results/fox_hpccg_tg/
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NFS_REPO="$(echo "$REPO_ROOT" | sed 's|^/home/|/nfs/home/|')"

readonly BIN="HPCCG_mpi_oss-notampi.bin"
readonly NREPS=3
readonly PARTITION="${PARTITION:-fox}"

# HSF configs: "NX:NY:NZ:NTASKS_LIST:MAXIT:CPUS"
# All use: tgenabled=1, lower=cs, upper=cs, flex=0, imm=false, bsf=4
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

main() {
    setup_slurm_env
    echo "=== HPCCG HSF (taskgroups) ==="

    for exp in "${EXPERIMENTS[@]}"; do
        IFS=':' read -r nx ny nz ntasks_str maxit cpus <<< "$exp"
        IFS=',' read -ra ntasks_arr <<< "$ntasks_str"

        for ntasks in "${ntasks_arr[@]}"; do
            export VVV_TG_ENABLED=1
            export VVV_LOWER_LVL=cs
            export VVV_UPPER_LVL=cs
            export VVV_AFF_FLEXIBLE=0
            export VVV_BS_FACTOR=4
            export VVV_MMAP_ENABLED=1
            export VVV_SPMV_TG_ENABLED=1
            export VVV_SPMV_POLICY=FIFO
            export VVV_SPMV_IMM=1
            export VVV_SPMV_TG_MULT=1
            export NOSV_CONFIG=nosv.toml
            export NOSV_CONFIG_OVERRIDE="topology.binding=inherit,hwcounters.backend=none,scheduler.immediate_successor=false,taskgroups.save_hierarchy=false"
            export SLURM_NTASKS_PER_NODE=1
            export SLURM_NPROCS=1
            export SRUN_CPUS_PER_TASK=$cpus

            export VVV_EXP_STR="nx${nx}_ny${ny}_nz${nz}_nt${ntasks}_maxit${maxit}_lower_cs_upper_cs_flex0_bsf4"

            local raw_dir="${REPO_ROOT}/reproduced_results/fox_hpccg_tg/results_final_3bs/raw/${VVV_EXP_STR}"
            mkdir -p "$raw_dir"

            cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${VVV_EXP_STR}",
    "nx": $nx, "ny": $ny, "nz": $nz, "ntasks": $ntasks,
    "maxit": $maxit, "tgenabled": 1,
    "lower": "cs", "upper": "cs", "flex": 0,
    "imm": "false", "ppn": 1,
    "threads": $cpus, "bsf": 4
}
METAEOF
            local nfs_raw="$(echo "$raw_dir" | sed 's|^/home/|/nfs/home/|')"

            echo "  [tg] nx=$nx ny=$ny nz=$nz ntasks=$ntasks cpus=$cpus"
            sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
                --switches=1 --export=ALL \
                -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
                --output="${nfs_raw}/job%A_%a.out" \
                --error="${nfs_raw}/job%A_%a.err" \
                "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$nx" "$ny" "$nz" "$maxit" "$ntasks"
        done
    done
    echo "All jobs submitted."
}

main
