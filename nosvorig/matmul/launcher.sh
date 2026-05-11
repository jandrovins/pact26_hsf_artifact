#!/usr/bin/env bash
set -u

# ============================================================================
# CONFIGURATION SECTION
# ============================================================================

# Script arguments
readonly OUTDIR_BASE=$1
readonly BIN=$2
readonly NFS_WORKDIR="$(pwd | sed 's|^/home/|/nfs/home/|')"

# Experiment parameters
readonly NREPS=7
readonly OVNI=none
readonly SLURM_PARTITION="fox"

# Topology configuration
declare -A UPPER_MAP=(
    [node]="node"
    [numa]="numa node"
    [cs]="cs numa node"
    [core]="core cs numa node"
)

# Experiments: Each entry is "nsize:msize:ts_list:its:ppn:cpus_per_task"
# nsize = N dimension, msize = M dimension, ts = tile sizes, its = iterations
# ppn = processes per node, cpus_per_task = cpus per process
readonly EXPERIMENTS=(
    #"6144:6144:64,96,128:20:1:96"
    #"24576:24576:64,96,128:3:8:24"
    "49152:49152:192,256,512,1024,2048:1:1:192"
)

# Levels to test
readonly LEVELS=("node")
readonly IMM_VALUES=("true")
readonly MMAP_VALUES=(1)
readonly NUMA_INTERLEAVED_VALUES=(1 0)

# ============================================================================
# HELPER FUNCTIONS
# ============================================================================

setup_prefix_dir() {
    local prefix_dir=$1
    mkdir -p "${prefix_dir}/src" "${prefix_dir}/experiments"
    for pattern in "*.c" "*.h" "Makefile" "*.toml" "*.sh" "*.job" "*.py"; do
        for f in $pattern; do [ -f "$f" ] && cp "$f" "${prefix_dir}/src/"; done
    done
    echo "Matmul launcher prefix: ${prefix_dir}"
}

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SLURM_EXCLUSIVE=""
    export SBATCH_EXCLUSIVE=""

    if [ "$OVNI" = "ovni" ]; then
        export SBATCH_TIMELIMIT="00:20:00"
    else
        export SBATCH_TIMELIMIT="00:03:00"
    fi
}

setup_nosv_env() {
    export NOSV_CONFIG=nosv.toml
    export NOSV_CONFIG_OVERRIDE="topology.binding=inherit"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.enabled=false"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.verbose=false"
    export NOSV_CONFIG_OVERRIDE+=",hwcounters.backend=none"
}

parse_experiment() {
    local exp=$1
    IFS=':' read -r EXP_NSIZE EXP_MSIZE EXP_TS_STR EXP_ITS EXP_PPN EXP_CPUS_PER_TASK <<< "$exp"
    IFS=',' read -ra EXP_TS_ARR <<< "$EXP_TS_STR"
}

run_experiment() {
    local nsize=$1 msize=$2 ts=$3 its=$4 imm=$5 cpus=$6
    local srunout=$7

    mkdir -p "$srunout"

    # Job naming
    export SLURM_JOB_NAME="matmul_${VVV_EXP_STR}"
    export SBATCH_JOB_NAME="${VVV_EXP_STR}"
    unset SBATCH_OUTPUT SBATCH_ERROR SRUN_OUTPUT SRUN_ERROR SLURM_JOB_ID

    local abs_out="${NFS_WORKDIR}/${srunout}"

    # Retry sbatch on failure with 30 second sleep
    local max_retries=100
    local retry_count=0
    while [ $retry_count -lt $max_retries ]; do
        sbatch -p "$SLURM_PARTITION" --array=1-${NREPS} \
            --output="${abs_out}/job%A_%a.out" \
            --error="${abs_out}/job%A_%a.err" \
            --switches=1 \
            -N "$SLURM_JOB_NUM_NODES" \
            --ntasks-per-node="$SLURM_NTASKS_PER_NODE" \
            --cpus-per-task="$SRUN_CPUS_PER_TASK" \
            ./submit-insalloc.job "$BIN" "$nsize" "$msize" "$its" "$ts" "$imm" "$OVNI" && break
        retry_count=$((retry_count + 1))
        echo "sbatch failed, retry $retry_count/$max_retries in 30 seconds..."
        sleep 30
    done
}

# ============================================================================
# MAIN EXECUTION
# ============================================================================

main() {
    local prefix_dir="out2/${OUTDIR_BASE}"
    setup_prefix_dir "$prefix_dir"

    setup_slurm_env
    setup_nosv_env

    for lvl in "${LEVELS[@]}"; do
        export VVV_LOWER_LVL=$lvl
        read -ra uppers <<< "${UPPER_MAP[$lvl]}"

        for upper in "${uppers[@]}"; do
            export VVV_UPPER_LVL=$upper
            export VVV_AFF_FLEXIBLE=$( [ "$upper" = "$lvl" ] && echo 0 || echo 1 )

            echo "Running with lower=$VVV_LOWER_LVL upper=$VVV_UPPER_LVL"

            tgenabled=0

            for imm in "${IMM_VALUES[@]}"; do
                for exp in "${EXPERIMENTS[@]}"; do
                    parse_experiment "$exp"

                    for ts in "${EXP_TS_ARR[@]}"; do
                        for mmap in "${MMAP_VALUES[@]}"; do
                            for numa_interleaved in "${NUMA_INTERLEAVED_VALUES[@]}"; do
                                # Set experiment variables
                                export VVV_MMAP_ENABLED=$mmap
                                export VVV_NUMA_INTERLEAVED=$numa_interleaved
                                export VVV_TG_ENABLED=$tgenabled
                                export VVV_NOSV_PAPI_ENABLED=0
                                export SLURM_NTASKS_PER_NODE=$EXP_PPN
                                export SLURM_NPROCS=$EXP_PPN
                                export SRUN_CPUS_PER_TASK=$EXP_CPUS_PER_TASK

                                export VVV_EXP_STR="imm${imm}_ppn${EXP_PPN}_cpuspertask${EXP_CPUS_PER_TASK}_nsize${EXP_NSIZE}_msize${EXP_MSIZE}_ts${ts}_its${EXP_ITS}_mmap${mmap}_numa${numa_interleaved}"

                                echo "  Running: $VVV_EXP_STR"

                                run_experiment "$EXP_NSIZE" "$EXP_MSIZE" "$ts" "$EXP_ITS" "$imm" "$EXP_CPUS_PER_TASK" \
                                    "${prefix_dir}/experiments/${VVV_EXP_STR}"
                            done
                        done
                    done
                done
            done
        done
    done

    echo "Matmul jobs submitted → out2/${OUTDIR_BASE}/"
}

main
