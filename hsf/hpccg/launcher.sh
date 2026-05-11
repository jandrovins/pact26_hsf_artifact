#!/usr/bin/env bash
set -u

# ============================================================================
# launcher_v2_base.sh — Reps-per-sbatch launcher
# ============================================================================
# Runs multiple repetitions in a tight bash loop inside a single sbatch job,
# loading the nix devshell only once per batch.
#
# Usage:
#   TOTAL_REPS=10 REPS_PER_SBATCH=10 ./launcher_v2_base.sh [outdir] [binary]
# ============================================================================

# ============================================================================
# CONFIGURATION SECTION
# ============================================================================

readonly OUTDIR_BASE="${1:-hpccg_ceiling_v2}"
readonly BIN="${2:-HPCCG_mpi_oss-notampi.bin}"

readonly TOTAL_REPS="${TOTAL_REPS:-1}"
readonly REPS_PER_SBATCH="${REPS_PER_SBATCH:-1}"
readonly N_BATCHES=$(( (TOTAL_REPS + REPS_PER_SBATCH - 1) / REPS_PER_SBATCH ))
readonly OVNI=ovni

# Slurm partition — override via environment: PARTITION=owl ./launcher_v2_base.sh ...
readonly PARTITION="${PARTITION:-fox}"

# Experiments: "NX:NY:NZ:MAX_ITER:NTASKS:CPUS:PPN:BSF"
# NX, NY, NZ: local sub-block dimensions per MPI rank
# NTASKS: comma-separated list of ntasks values to sweep
# CPUS: cpus-per-task (= threads per rank)
# PPN: processes (MPI ranks) per node
# BSF: comma-separated list of BS_FACTOR values to sweep
readonly EXPERIMENTS=(
    # 1 rank, 192 cores, nz=1536
    #"384:384:1536:40:1536,3072:192:1:4,16,32"
    # 8 ranks, 24 cores/rank, nz=192
    #"384:384:192:40:192,384:24:8:4,16,32"

    "288:192:768:100:1536:96:1:4"
    #"288:192:192:100:384:24:4:16"
    #"288:192:768:100:4608:96:1:4"
    #"288:192:192:100:96,384,1536:24:4:4"
    #"384:384:1536:40:9216,12288:192:1:4"
    #"384:384:192:40:192,768,3072:24:8:4"
    "384:384:192:40:3072:192:1:4"
)

# Immediate-successor mode for nOS-V scheduler
readonly IMM_VALUES=("false")

# Taskgroup / affinity settings
readonly TG_ENABLED_VALUES=(1)

# Map lower level -> possible upper levels for affinity sweep.
# When upper == lower, AFF_FLEXIBLE=0; otherwise AFF_FLEXIBLE=1.
declare -A UPPER_MAP=(
    [node]="node"
    [numa]="numa"
    [cs]="cs"
    [core]="core cs"
)

# Which lower levels to sweep (keys into UPPER_MAP)
readonly LEVELS=("cs")

# SpMV taskgroup settings
readonly SPMV_TG_ENABLED=1
readonly SPMV_POLICY="FIFO"
readonly SPMV_IMM=1
readonly SPMV_TG_MULT=1

# Memory-mapped allocator
readonly MMAP_ENABLED=1

# HW counters
readonly HWC_BACKEND="none"
readonly NOSV_PAPI_ENABLED=0

# Source patterns to snapshot into the output prefix
readonly SRC_PATTERNS=("*.cpp" "*.hpp" "*.h" "Makefile" "*.toml" "*.sh" "*.job" "*.md")

# ============================================================================
# HELPER FUNCTIONS
# ============================================================================

readonly NFS_WORKDIR="$(pwd | sed 's|^/home/|/nfs/home/|')"

setup_prefix_dir() {
    local prefix_dir=$1
    mkdir -p "${prefix_dir}/src" "${prefix_dir}/experiments"
    for pattern in "${SRC_PATTERNS[@]}"; do
        for f in $pattern; do [ -f "$f" ] && cp "$f" "${prefix_dir}/src/"; done
    done
    [ -d "src" ] && cp -r src "${prefix_dir}/src/src_tree" 2>/dev/null || true
    [ -d "include" ] && cp -r include "${prefix_dir}/src/include" 2>/dev/null || true
    echo "Created prefix directory: ${prefix_dir}"
    echo "  Source snapshot: ${prefix_dir}/src/"
}

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SLURM_EXCLUSIVE=""
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="03:00:00"
}

setup_nosv_env() {
    if [ "$PARTITION" = "owl" ]; then
        export NOSV_CONFIG=nosv-owl.toml
    else
        export NOSV_CONFIG=nosv.toml
    fi
    export NOSV_CONFIG_OVERRIDE="topology.binding=inherit"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.enabled=false"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.verbose=false"
    export NOSV_CONFIG_OVERRIDE+=",hwcounters.backend=${HWC_BACKEND}"
    export NOSV_CONFIG_OVERRIDE+=",taskgroups.save_hierarchy=false"
    export NOSV_CONFIG_OVERRIDE+=",taskgroups.dot_filename=tg_hierarchy.dot"
}

parse_experiment() {
    local exp=$1
    IFS=':' read -r EXP_NX EXP_NY EXP_NZ EXP_MAX_ITER EXP_NTASKS_STR EXP_CPUS EXP_PPN EXP_BSF_STR <<< "$exp"
    IFS=',' read -ra EXP_NTASKS_ARR <<< "$EXP_NTASKS_STR"
    IFS=',' read -ra EXP_BSF_ARR <<< "$EXP_BSF_STR"
}

run_experiment() {
    local nx=$1 ny=$2 nz=$3 max_iter=$4 ntasks=$5 imm=$6 ppn=$7 srunout=$8
    mkdir -p "$srunout"
    local abs_out="${NFS_WORKDIR}/${srunout}"

    for ((batch=1; batch<=N_BATCHES; batch++)); do
        export SLURM_JOB_NAME="${OUTDIR_BASE}_${VVV_EXP_STR}"
        export SBATCH_JOB_NAME="${OUTDIR_BASE}_${VVV_EXP_STR}"
        unset SBATCH_OUTPUT SBATCH_ERROR SRUN_OUTPUT SRUN_ERROR SLURM_JOB_ID

        local max_retries=100 retry_count=0
        while [ $retry_count -lt $max_retries ]; do
            sbatch -p "$PARTITION" \
                --output="${abs_out}/batch${batch}_job%j.out" \
                --error="${abs_out}/batch${batch}_job%j.err" \
                --switches=1 --export=ALL \
                -N "$SLURM_JOB_NUM_NODES" \
                --ntasks-per-node="$SLURM_NTASKS_PER_NODE" \
                --cpus-per-task="$SRUN_CPUS_PER_TASK" \
                ./submit_loop.job "$BIN" "$nx" "$ny" "$nz" \
                    "$max_iter" "$ntasks" "$imm" "$REPS_PER_SBATCH" "$OVNI" && break
            retry_count=$((retry_count + 1))
            echo "sbatch failed, retry $retry_count/$max_retries in 30s..."
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

    # Export common environment
    export VVV_SPMV_TG_ENABLED=$SPMV_TG_ENABLED
    export VVV_SPMV_POLICY=$SPMV_POLICY
    export VVV_SPMV_IMM=$SPMV_IMM
    export VVV_SPMV_TG_MULT=$SPMV_TG_MULT
    export VVV_MMAP_ENABLED=$MMAP_ENABLED
    export VVV_NOSV_PAPI_ENABLED=$NOSV_PAPI_ENABLED

    local total_jobs=0

    for tge in "${TG_ENABLED_VALUES[@]}"; do
        export VVV_TG_ENABLED=$tge

    for lvl in "${LEVELS[@]}"; do
        export VVV_LOWER_LVL=$lvl
        read -ra uppers <<< "${UPPER_MAP[$lvl]}"

        for upper in "${uppers[@]}"; do
            export VVV_UPPER_LVL=$upper
            export VVV_AFF_FLEXIBLE=$( [ "$upper" = "$lvl" ] && echo 0 || echo 1 )

            for exp in "${EXPERIMENTS[@]}"; do
                parse_experiment "$exp"

                export SLURM_NTASKS_PER_NODE=$EXP_PPN
                export SLURM_NPROCS=$EXP_PPN
                export SRUN_CPUS_PER_TASK=$EXP_CPUS

                for imm in "${IMM_VALUES[@]}"; do
                    for ntasks in "${EXP_NTASKS_ARR[@]}"; do
                        for bsf in "${EXP_BSF_ARR[@]}"; do
                            export VVV_BS_FACTOR=$bsf

                            export VVV_EXP_STR="tge${tge}_imm${imm}_lower${lvl}_upper${upper}_flex${VVV_AFF_FLEXIBLE}_ppn${EXP_PPN}_nx${EXP_NX}_ny${EXP_NY}_nz${EXP_NZ}_maxit${EXP_MAX_ITER}_ntasks${ntasks}_bsf${bsf}_cpuspertask${EXP_CPUS}"

                            echo "  [Submit] $VVV_EXP_STR (${N_BATCHES} batches × ${REPS_PER_SBATCH} reps)"

                            run_experiment "$EXP_NX" "$EXP_NY" "$EXP_NZ" "$EXP_MAX_ITER" \
                                "$ntasks" "$imm" "$EXP_PPN" \
                                "${prefix_dir}/experiments/${VVV_EXP_STR}"

                            total_jobs=$((total_jobs + N_BATCHES))
                        done
                    done
                done
            done
        done
    done
    done  # tge

    echo ""
    echo "All jobs submitted: ${total_jobs} sbatch jobs → ${prefix_dir}/"
}

main
