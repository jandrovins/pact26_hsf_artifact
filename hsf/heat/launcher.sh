#!/usr/bin/env bash
# =============================================================================
# launcher_heat.sh — HPCCG-style launcher for heat-tg2 benchmark
#
# Usage: ./launcher_heat.sh <outdir_base> <binary>
#
# Sweeps tglib task-group configurations for the 2D heat Gauss-Seidel solver.
# N=49152  → 192 CPUs (full node, 2 sockets)
# N=12288  → 96  CPUs (first socket only, NUMA nodes 0-3)
# =============================================================================
set -u

readonly OUTDIR_BASE="${1:-heat_tglib_results}"
readonly BIN="${2:-02.heat_ompss2.bin}"
readonly NFS_WORKDIR="$(pwd | sed 's|^/home/|/nfs/home/|')"

# Experiment format: "N:BS_LIST:ITS:WARMUP:CPUS:PPN"
# N=49152 → 192 CPUs; N=12288 → 96 CPUs (socket 0)
readonly EXPERIMENTS=(
    "49152:256,512:60:1:192:1"
    #"12288:768,1024:800:1:96:1"
)

readonly TG_ENABLED_VALUES=(1)
readonly LEVELS=("cs")
readonly POLICY_VALUES=("PRIO")
readonly DIAGS_VALUES=("1")
readonly BLOCKFUNC_VALUES=("0")   # 0=row, 1=diagonal
readonly PRIORITY_VALUES=("0")   # 0=iteration, 3=(R+C)*10000+R
readonly TOTAL_REPS=10
readonly REPS_PER_SBATCH=10

readonly PARTITION="${PARTITION:-fox}"

declare -A UPPER_MAP=(
    [node]="node"
    [numa]="numa node"
    [cs]="numa"
    [core]="core cs"
)

# ── helpers ──────────────────────────────────────────────────────────────────

setup_prefix_dir() {
    local prefix_dir=$1
    mkdir -p "${prefix_dir}/src" "${prefix_dir}/experiments"
    for pattern in "*.c" "*.h" "Makefile" "*.toml" "*.sh" "*.job" "*.md" "*.py"; do
        for f in $pattern; do [ -f "$f" ] && cp "$f" "${prefix_dir}/src/"; done
    done
    echo "Heat launcher prefix: ${prefix_dir}"
}

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SLURM_EXCLUSIVE=""
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:30:00"
}

setup_nosv_env() {
    export NOSV_CONFIG=nosv.toml
    export NOSV_CONFIG_OVERRIDE="topology.binding=inherit"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.enabled=false"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.verbose=false"
    export NOSV_CONFIG_OVERRIDE+=",hwcounters.backend=none"
    export NOSV_CONFIG_OVERRIDE+=",taskgroups.save_hierarchy=false"
}

parse_experiment() {
    local exp=$1
    IFS=':' read -r EXP_N EXP_BS_STR EXP_ITS EXP_WARMUP EXP_CPUS EXP_PPN <<< "$exp"
    IFS=',' read -ra EXP_BS_ARR <<< "$EXP_BS_STR"
}

run_experiment() {
    local n=$1 bs=$2 its=$3 warmup=$4 cpus=$5 ppn=$6 srunout=$7
    mkdir -p "$srunout"
    local abs_out="${NFS_WORKDIR}/${srunout}"
    export SLURM_JOB_NAME="${OUTDIR_BASE}_${VVV_EXP_STR}"
    export SBATCH_JOB_NAME="${OUTDIR_BASE}_${VVV_EXP_STR}"
    unset SBATCH_OUTPUT SBATCH_ERROR SRUN_OUTPUT SRUN_ERROR SLURM_JOB_ID

    # Each array element runs REPS_PER_SBATCH inner reps.
    # Total reps = num_jobs * REPS_PER_SBATCH >= TOTAL_REPS
    local num_jobs=$(( (TOTAL_REPS + REPS_PER_SBATCH - 1) / REPS_PER_SBATCH ))
    local max_retries=100 retry_count=0
    while [ $retry_count -lt $max_retries ]; do
        sbatch -p "$PARTITION" --array=1-${num_jobs} \
            --output="${abs_out}/job%A_%a.out" \
            --error="${abs_out}/job%A_%a.err" \
            --switches=1 --export=ALL \
            -N "$SLURM_JOB_NUM_NODES" \
            --ntasks-per-node="$ppn" \
            --cpus-per-task="$cpus" \
            ./submit_heat.job "$BIN" "$n" "$bs" "$its" "$warmup" "$cpus" "$REPS_PER_SBATCH" && break
        retry_count=$((retry_count + 1))
        echo "sbatch failed, retry $retry_count/$max_retries in 30s..."
        sleep 30
    done
}

# ── main ─────────────────────────────────────────────────────────────────────

main() {
    local prefix_dir="out/${OUTDIR_BASE}"
    setup_prefix_dir "$prefix_dir"
    setup_slurm_env
    setup_nosv_env

    for tge in "${TG_ENABLED_VALUES[@]}"; do
        export VVV_TG_ENABLED=$tge

        for lvl in "${LEVELS[@]}"; do
            export VVV_LOWER_LVL=$lvl
            read -ra uppers <<< "${UPPER_MAP[$lvl]}"

            for upper in "${uppers[@]}"; do
                export VVV_UPPER_LVL=$upper
                export VVV_AFF_FLEXIBLE=$( [ "$upper" = "$lvl" ] && echo 0 || echo 1 )

                for pol in "${POLICY_VALUES[@]}"; do
                    export VVV_TG_POLICY=$pol

                    for diags in "${DIAGS_VALUES[@]}"; do
                        export VVV_DIAGS=$diags

                        for prio in "${PRIORITY_VALUES[@]}"; do
                            export VVV_PRIORITY=$prio

                            for bf in "${BLOCKFUNC_VALUES[@]}"; do
                                export VVV_BLOCKFUNC=$bf

                                for exp in "${EXPERIMENTS[@]}"; do
                                    parse_experiment "$exp"

                                    for bs in "${EXP_BS_ARR[@]}"; do
                                        # Socket binding for N=12288: restrict to CPUs 0-95 (socket 0)
                                        local topo_bind="inherit"
                                        if [ "$EXP_CPUS" = "96" ]; then
                                            topo_bind="0-95"
                                            export NOSV_CONFIG_OVERRIDE="topology.binding=${topo_bind},monitoring.enabled=false,monitoring.verbose=false,hwcounters.backend=none,taskgroups.save_hierarchy=false"
                                        else
                                            export NOSV_CONFIG_OVERRIDE="topology.binding=inherit,monitoring.enabled=false,monitoring.verbose=false,hwcounters.backend=none,taskgroups.save_hierarchy=false"
                                        fi

                                        export VVV_EXP_STR="tge${tge}_lower${lvl}_upper${upper}_flex${VVV_AFF_FLEXIBLE}_policy${pol}_n${EXP_N}_bs${bs}_its${EXP_ITS}_diags${diags}_prio${prio}_bf${bf}"
                                        echo "  [Heat] $VVV_EXP_STR"

                                        run_experiment "$EXP_N" "$bs" "$EXP_ITS" "$EXP_WARMUP" \
                                            "$EXP_CPUS" "$EXP_PPN" \
                                            "${prefix_dir}/experiments/${VVV_EXP_STR}"
                                    done
                                done
                            done
                        done
                    done
                done
            done
        done
    done

    echo "Heat jobs submitted → out/${OUTDIR_BASE}/"
}

main
