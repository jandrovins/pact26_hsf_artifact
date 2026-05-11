#!/usr/bin/env bash
set -u

# ============================================================================
# CONFIGURATION SECTION
# ============================================================================

# Script arguments
readonly OUTDIR_BASE=$1
readonly BIN=$2

# Experiment parameters
readonly NREPS=10
readonly OVNI=none
readonly SLURM_PARTITION="fox"

# Topology configuration
declare -A UPPER_MAP=(
    [node]="node"
    [numa]="numa node"
    [cs]="cs numa node"
    [core]="core cs numa node"
)

# Experiments: Each entry is "ntasks_list:ppn:cpus_per_task:nx:ny:nz:iterations"
# ntasks = number of tasks (will be divided by ppn internally)
# ppn = processes per node
# cpus_per_task = cpus per process
# nx, ny, nz = grid dimensions
# iterations = number of iterations
readonly EXPERIMENTS=(
    "3072,4608:1:96:288:192:768:100"
    #"3072,4608:4:24:288:192:192:100"
    "9216,12288:1:192:384:384:1536:40"
    #"4608,6144:8:24:384:384:192:40"
)

# Levels to test
readonly LEVELS=("node")
readonly IMM_VALUES=("true")
readonly MMAP_VALUES=(1)
readonly NUMA_INTERLEAVED_VALUES=(0 1)

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
    echo "Created prefix directory: ${prefix_dir}"
    echo "  Source snapshot: ${prefix_dir}/src/"
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
        export SBATCH_TIMELIMIT="00:20:00"
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
    IFS=':' read -r EXP_NTASKS_STR EXP_PPN EXP_CPUS_PER_TASK EXP_NX EXP_NY EXP_NZ EXP_ITERATIONS <<< "$exp"
    IFS=',' read -ra EXP_NTASKS_ARR <<< "$EXP_NTASKS_STR"
}

run_experiment() {
    local ntasks=$1 imm=$2 ppn=$3 cpus_per_task=$4
    local nx=$5 ny=$6 nz=$7 iterations=$8
    local srunout=$9

    mkdir -p "$srunout"
    local abs_out="${NFS_WORKDIR}/${srunout}"

    # Job naming
    export SLURM_JOB_NAME="hpccg_${VVV_EXP_STR}"
    export SBATCH_JOB_NAME="${VVV_EXP_STR}"
    unset SBATCH_OUTPUT SBATCH_ERROR SRUN_OUTPUT SRUN_ERROR SLURM_JOB_ID

    local max_retries=100 retry_count=0
    while [ $retry_count -lt $max_retries ]; do
        sbatch -p "$SLURM_PARTITION" --array=1-${NREPS} \
            --output="${abs_out}/job%A_%a.out" \
            --error="${abs_out}/job%A_%a.err" \
            --switches=1 \
            -N "$SLURM_JOB_NUM_NODES" \
            --ntasks-per-node="$SLURM_NTASKS_PER_NODE" \
            --cpus-per-task="$SRUN_CPUS_PER_TASK" \
            ./submit-insalloc.job "$BIN" "$ntasks" "$imm" "$OVNI" "$nx" "$ny" "$nz" "$iterations" && break
        retry_count=$((retry_count + 1))
        echo "sbatch failed, retry $retry_count/$max_retries in 30s..."
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

                    local ppn=$EXP_PPN
                    local cpus_per_task=$EXP_CPUS_PER_TASK
                    local nx=$EXP_NX
                    local ny=$EXP_NY
                    local nz=$EXP_NZ
                    local iterations=$EXP_ITERATIONS

                    for ntasks_base in "${EXP_NTASKS_ARR[@]}"; do
                        for mmap in "${MMAP_VALUES[@]}"; do
                            for numa_interleaved in "${NUMA_INTERLEAVED_VALUES[@]}"; do
                                local ntasks=$((ntasks_base / ppn))

                                # Set experiment variables
                                export VVV_MMAP_ENABLED=$mmap
                                export VVV_NUMA_INTERLEAVED=$numa_interleaved
                                export VVV_TG_ENABLED=$tgenabled
                                export VVV_NOSV_PAPI_ENABLED=0
                                export SLURM_NTASKS_PER_NODE=$ppn
                                export SLURM_NPROCS=$ppn
                                export SRUN_CPUS_PER_TASK=$cpus_per_task

                                export VVV_EXP_STR="imm${imm}_ppn${ppn}_cpuspertask${cpus_per_task}_nx${nx}_ny${ny}_nz${nz}_its${iterations}_ntasks${ntasks}_mmap${mmap}_numainterleaved${numa_interleaved}_tgenabled${tgenabled}_lower${lvl}_upper${upper}_affflex${VVV_AFF_FLEXIBLE}"

                                echo "  Running: $VVV_EXP_STR"

                                run_experiment "$ntasks" "$imm" "$ppn" "$cpus_per_task" \
                                    "$nx" "$ny" "$nz" "$iterations" \
                                    "${prefix_dir}/experiments/${VVV_EXP_STR}"
                            done
                        done
                    done
                done
            done
        done
    done
}

main
