#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce nosvorig Heat results (orig + init)
#
# Results go to $REPO_ROOT/reproduced_results/fox_heat_orig/
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
readonly BIN="02.heat_ompss2_prio.bin"
readonly NREPS="${NREPS:-3}"
readonly PARTITION="${PARTITION:-fox}"
readonly WARMUP=1

# Configs from original results:
# Format: "N:BS_LIST:ITS:CPUS"
readonly EXPERIMENTS=(
    "12288:256,768,1024:800:96"
    "49152:256,512,2048:60:192"
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
        local numa_val="1"
    else
        local results_subdir="results_final_3bs"
        local numa_val=""
    fi

    for exp in "${EXPERIMENTS[@]}"; do
        IFS=':' read -r n bs_str its cpus <<< "$exp"
        IFS=',' read -ra bs_arr <<< "$bs_str"

        for bs in "${bs_arr[@]}"; do
            local topo_bind="inherit"
            if [ "$cpus" = "96" ]; then
                topo_bind="0-95"
            fi

            export VVV_MMAP_ENABLED=1
            export VVV_PRIORITY=1
            export VVV_NUMA_INTERLEAVED=$numa_interleaved
            export NOSV_CONFIG=nosv.toml
            export NOSV_CONFIG_OVERRIDE="topology.binding=${topo_bind},monitoring.enabled=false,monitoring.verbose=false,hwcounters.backend=none,scheduler.immediate_successor=true"

            export VVV_EXP_STR="n${n}_bs${bs}_its${its}_imm_true_mmap1_prio1_numa${numa_val}"

            local raw_dir="${REPO_ROOT}/reproduced_results/fox_heat_orig/${results_subdir}/raw/${VVV_EXP_STR}"
            mkdir -p "$raw_dir"

            cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${VVV_EXP_STR}",
    "n": $n, "bs": $bs, "its": $its,
    "imm": "true", "mmap": 1, "prio": 1, "numa": "${numa_val}"
}
METAEOF
            local nfs_raw="$(nfsmap "$raw_dir")"
            echo "  [$variant] n=$n bs=$bs its=$its numa=$numa_val"
            sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
                --switches=1 --export=ALL \
                -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
                --output="${nfs_raw}/job%A_%a.out" \
                --error="${nfs_raw}/job%A_%a.err" \
                "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$n" "$bs" "$its" "$WARMUP" "$cpus"
        done
    done
}

main() {
    setup_slurm_env
    echo "=== Heat nosvorig: orig ==="
    run_variant "orig" 0
    echo "=== Heat nosvorig: init (NUMA interleaved) ==="
    run_variant "init" 1
    echo "All jobs submitted."
}

main
