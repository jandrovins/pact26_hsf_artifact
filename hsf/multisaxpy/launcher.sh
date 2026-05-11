#!/usr/bin/env bash
set -u

# ============================================================================
# Multisaxpy Launcher — Taskgroup Experiments
# ============================================================================
# Sweeps over taskgroup configurations: topology levels (node, numa, cs, core)
# and scheduling policies (PRIO, FIFO) to reproduce evaluation results.
#
# Usage:
#   ./launcher_new.sh <outdir_label> <binary>
#
# Example:
#   ./launcher_new.sh multisaxpy_tg b6_multisaxpy_nodes
# ============================================================================

# ============================================================================
# CONFIGURATION SECTION
# ============================================================================

readonly OUTDIR_BASE=$1
readonly BIN=$2

readonly TOTAL_REPS=10
readonly REPS_PER_SBATCH=10
readonly N_BATCHES=$(( (TOTAL_REPS + REPS_PER_SBATCH - 1) / REPS_PER_SBATCH ))
readonly OVNI=none

# Slurm partition
readonly PARTITION="${PARTITION:-fox}"

# NFS-absolute path to this directory (needed for sbatch --output on compute nodes)
readonly NFS_WORKDIR="$(pwd | sed 's|^/home/|/nfs/home/|')"

# Experiments: "N:TS:ITERATIONS:CPUS:PPN:SOCKETS"
#   CPUS    = --cpus-per-task for srun (nOS-V sees this many cores via topology.binding=inherit)
#   SOCKETS = --sockets-per-node for sbatch; empty string = no constraint (all sockets)
#
# Blocksize reference (original TS=58880, ~920 KiB/block, ~90% of 1 MiB L2):
#   TS/2  = 29440  (~460 KiB)
#   TS/4  = 14720  (~230 KiB)
#   TS*2  = 117760 (~1840 KiB, spills into L3)
#
# N adjusted to be exact multiple of each TS:
#   Full-node TS=58880/29440/14720 : 771740160 (=13107x58880)
#   Full-node TS=117760            : 771681280 (=6553x117760)
#   Half-node TS=58880/29440/14720 : 385840640 (=6553x58880)
#   Half-node TS=117760            : 385781760 (=3276x117760)
readonly EXPERIMENTS=(
    # --- Full node (192 cores, 2 sockets) --- SOCKETS field empty = no constraint
    #"771740160:58880:100:192:1:"    # original TS
    #"771740160:29440:100:192:1:"    # TS/2
    #"771740160:14720:100:192:1:"    # TS/4
    #"771681280:117760:100:192:1:"   # TS*2  (N=6553x117760)
    #"771740160:14720:100:192:1:"    # TS/4
    #"771740160:29440:100:192:1:"    # TS/2
    "771740160:200974:100:192:1:"    # N/384
    "771740160:401948:100:192:1:"    # N/192

    # --- Half node (96 cores, 1 socket, ~half N) ---
    #"56524800:14720:1000:96:1:1"    # original TS  (N=6553x58880)
    #"56524800:29440:1000:96:1:1"    # TS/2
    #"56524800:58880:1000:96:1:1"    # TS/4
    #"56524800:117760:1000:96:1:1"   # TS*2  (N=3276x117760)
    #"56524800:14720:1000:96:1:1"     # TS/4
    #"56524800:29440:1000:96:1:1"     # TS/2
    "56524800:294400:1000:96:1:1"    # N/192
    "56524800:588800:1000:96:1:1"    # N/96
)

# Taskgroup configurations to sweep
# Format: "LOWER_LVL:POLICY:LABEL"
readonly TG_CONFIGS=(
    "node:PRIO:prio_per_node"
    "numa:PRIO:prio_per_numa"
    "cs:PRIO:prio_per_cs"
    "core:PRIO:prio_per_core"
)

# Source patterns to snapshot into the output prefix
readonly SRC_PATTERNS=("*.cpp" "*.h" "Makefile" "*.toml" "*.sh" "*.job" "*.md" "*.py")

# ============================================================================
# HELPER FUNCTIONS
# ============================================================================

setup_prefix_dir() {
    local prefix_dir=$1
    mkdir -p "${prefix_dir}/src"
    mkdir -p "${prefix_dir}/experiments"
    for pattern in "${SRC_PATTERNS[@]}"; do
        for f in $pattern; do
            [ -f "$f" ] && cp "$f" "${prefix_dir}/src/"
        done
    done
    echo "Created prefix directory: ${prefix_dir}"
    echo "  Source snapshot: ${prefix_dir}/src/"
}

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SLURM_EXCLUSIVE=""
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:15:00"
}

setup_nosv_env() {
    export NOSV_CONFIG=nosv.toml
    export NOSV_CONFIG_OVERRIDE="topology.binding=inherit"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.enabled=false"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.verbose=false"
    export NOSV_CONFIG_OVERRIDE+=",hwcounters.backend=none"
    export NOSV_CONFIG_OVERRIDE+=",taskgroups.save_hierarchy=false"
    export NOSV_CONFIG_OVERRIDE+=",taskgroups.dot_filename=tg_hierarchy.dot"
}

parse_experiment() {
    local exp=$1
    IFS=':' read -r EXP_N EXP_TS EXP_ITS EXP_CPUS EXP_PPN EXP_SOCKETS <<< "$exp"
}

parse_tg_config() {
    local config=$1
    IFS=':' read -r TG_LOWER TG_POLICY TG_LABEL <<< "$config"
}

run_experiment() {
    local nsize=$1 ts=$2 its=$3 ppn=$4 sockets=$5
    local srunout=$6
    local abs_out="${NFS_WORKDIR}/${srunout}"
    mkdir -p "$srunout"

    # Optional socket constraint
    local socket_flag=""
    [ -n "$sockets" ] && socket_flag="--sockets-per-node=${sockets}"

    for ((batch=1; batch<=N_BATCHES; batch++)); do
        export SLURM_JOB_NAME="${OUTDIR_BASE}_${VVV_EXP_STR}"
        export SBATCH_JOB_NAME="${OUTDIR_BASE}_${VVV_EXP_STR}"
        unset SBATCH_OUTPUT SBATCH_ERROR SRUN_OUTPUT SRUN_ERROR SLURM_JOB_ID

        local max_retries=100
        local retry_count=0
        while [ $retry_count -lt $max_retries ]; do
            sbatch -p "$PARTITION" \
                --output="${abs_out}/batch${batch}_job%j.out" \
                --error="${abs_out}/batch${batch}_job%j.err" \
                --switches=1 \
                --export=ALL \
                -N "$SLURM_JOB_NUM_NODES" \
                --ntasks-per-node="$SLURM_NTASKS_PER_NODE" \
                --cpus-per-task="$SRUN_CPUS_PER_TASK" \
                ${socket_flag} \
                ./submit-insalloc-new.job "$BIN" "$nsize" "$ts" "$its" "$REPS_PER_SBATCH" "$OVNI" && break
            retry_count=$((retry_count + 1))
            echo "sbatch failed, retry $retry_count/$max_retries in 30 seconds..."
            sleep 30
        done
    done
}

# ============================================================================
# MAIN EXECUTION
# ============================================================================

main() {
    local prefix_dir="out/${OUTDIR_BASE}"
    setup_prefix_dir "$prefix_dir"
    setup_slurm_env
    setup_nosv_env

    echo "TOTAL_REPS=${TOTAL_REPS}, REPS_PER_SBATCH=${REPS_PER_SBATCH}, N_BATCHES=${N_BATCHES}"

    for config in "${TG_CONFIGS[@]}"; do
        parse_tg_config "$config"

        export VVV_TG_ENABLED=1
        export VVV_LOWER_LVL=$TG_LOWER
        export VVV_UPPER_LVL=$TG_LOWER
        export VVV_AFF_FLEXIBLE=0
        export VVV_TG_POLICY=$TG_POLICY

        for exp in "${EXPERIMENTS[@]}"; do
            parse_experiment "$exp"

            export SLURM_NTASKS_PER_NODE=$EXP_PPN
            export SLURM_NPROCS=$EXP_PPN
            export SRUN_CPUS_PER_TASK=$EXP_CPUS

            export VVV_EXP_STR="cpus${EXP_CPUS}_ppn${EXP_PPN}_n${EXP_N}_ts${EXP_TS}_its${EXP_ITS}_lower${TG_LOWER}_policy${TG_POLICY}_label${TG_LABEL}"

            echo "  Running: $VVV_EXP_STR"

            run_experiment "$EXP_N" "$EXP_TS" "$EXP_ITS" "$EXP_PPN" "$EXP_SOCKETS" \
                "${prefix_dir}/experiments/${VVV_EXP_STR}"
        done
    done

    echo ""
    echo "All jobs submitted. Output directory: ${prefix_dir}/"
}

main
