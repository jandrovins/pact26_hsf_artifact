#!/usr/bin/env bash
set -u

# ============================================================================
# CONFIGURATION SECTION
# ============================================================================

# Script arguments
readonly OUTDIR_BASE=$1
readonly BIN=$2

# Experiment parameters
readonly NREPS=3
readonly OVNI=none
readonly PPN=1
readonly SLURM_PARTITION="fox"

# Topology configuration
declare -A UPPER_MAP=(
    [node]="node"
    [numa]="numa node"
    [cs]="cs numa"
    [core]="core cs numa node"
)

# Experiments: Each entry is "nsize:ts_list:cpus_per_task"
readonly EXPERIMENTS=(
    "33792:512:192"
)

# Levels to test
readonly LEVELS=("numa")
readonly IMM_VALUES=("false")
readonly MMAP_VALUES=(0)
readonly PRIORITY_VALUES=(1)

# VVV_CHOL_GEMM_TILES_PER_BLOCK
readonly GEMM_TPB_VALUES=(48)

# HWC backend: "none" or "papi"
readonly HWC_BACKENDS=("none" "papi")

# Source files to snapshot
readonly SRC_PATTERNS=("*.c" "*.h" "Makefile" "*.toml" "*.sh" "*.job" "*.md" "*.py")

# ============================================================================
# HELPER FUNCTIONS
# ============================================================================

setup_prefix_dir() {
    local prefix_dir=$1

    mkdir -p "${prefix_dir}/src"
    mkdir -p "${prefix_dir}/experiments"

    # Copy source files into src/ snapshot
    for pattern in "${SRC_PATTERNS[@]}"; do
        # Use nullglob-safe copy
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

    if [ "$OVNI" = "ovni" ]; then
        export SBATCH_TIMELIMIT="00:20:00"
    else
        export SBATCH_TIMELIMIT="00:05:00"
    fi
}

setup_nosv_env() {
    local hwc_backend=$1
    export NOSV_CONFIG=nosv.toml
    export NOSV_CONFIG_OVERRIDE="topology.binding=inherit"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.enabled=false"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.verbose=false"
    export NOSV_CONFIG_OVERRIDE+=",hwcounters.backend=${hwc_backend}"
    export NOSV_CONFIG_OVERRIDE+=",taskgroups.save_hierarchy=false"
    export NOSV_CONFIG_OVERRIDE+=",taskgroups.dot_filename=tg_hierarchy.dot"
}

parse_experiment() {
    local exp=$1
    IFS=':' read -r EXP_NSIZE EXP_TS_STR EXP_CPUS <<< "$exp"
    IFS=',' read -ra EXP_TS_ARR <<< "$EXP_TS_STR"
}

run_experiment() {
    local nsize=$1 ts=$2 imm=$3 cpus=$4
    local srunout=$5

    mkdir -p "$srunout"

    # Job naming
    export SLURM_JOB_NAME="${OUTDIR_BASE}_${VVV_EXP_STR}"
    export SBATCH_JOB_NAME="${OUTDIR_BASE}_${VVV_EXP_STR}"
    export SBATCH_OUTPUT="${srunout}/job%J.out"
    export SBATCH_ERROR="${srunout}/job%J.err"
    export SRUN_OUTPUT="$SBATCH_OUTPUT"
    export SRUN_ERROR="$SBATCH_ERROR"

    unset SLURM_JOB_ID
    sbatch -p "$SLURM_PARTITION" --array=1-${NREPS} \
        --switches=1 \
        --export=ALL \
        -N "$SLURM_JOB_NUM_NODES" \
        --ntasks-per-node="$SLURM_NTASKS_PER_NODE" \
        --cpus-per-task="$SRUN_CPUS_PER_TASK" \
        ./submit-insalloc.job "$BIN" "$nsize" "$ts" "$imm" "$OVNI"
}

# ============================================================================
# MAIN EXECUTION
# ============================================================================

main() {
    local prefix_dir="out/${OUTDIR_BASE}"

    setup_prefix_dir "$prefix_dir"
    setup_slurm_env

    for hwc_backend in "${HWC_BACKENDS[@]}"; do
        setup_nosv_env "$hwc_backend"

        for lvl in "${LEVELS[@]}"; do
            export VVV_LOWER_LVL=$lvl
            read -ra uppers <<< "${UPPER_MAP[$lvl]}"

            for upper in "${uppers[@]}"; do
                export VVV_UPPER_LVL=$upper
                export VVV_AFF_FLEXIBLE=$( [ "$upper" = "$lvl" ] && echo 0 || echo 1 )

                echo "Running with lower=$VVV_LOWER_LVL upper=$VVV_UPPER_LVL hwc=$hwc_backend"

                tgenabled=1

                for imm in "${IMM_VALUES[@]}"; do
                    for exp in "${EXPERIMENTS[@]}"; do
                        parse_experiment "$exp"

                        for ts in "${EXP_TS_ARR[@]}"; do
                            for mmap in "${MMAP_VALUES[@]}"; do
                                for use_priority in "${PRIORITY_VALUES[@]}"; do
                                    for gemm_tpb in "${GEMM_TPB_VALUES[@]}"; do
                                        export VVV_CHOL_PRIORITY_ENABLED=$use_priority
                                        export VVV_CHOL_GEMM_TILES_PER_BLOCK=$gemm_tpb
                                        # Set experiment variables
                                        export VVV_MMAP_ENABLED=$mmap
                                        export VVV_TG_ENABLED=$tgenabled
                                        export SLURM_NTASKS_PER_NODE=$PPN
                                        export SLURM_NPROCS=$PPN
                                        export SRUN_CPUS_PER_TASK=$((EXP_CPUS/PPN))

                                        export VVV_EXP_STR="imm${imm}_ppn${PPN}_n${EXP_NSIZE}_ts${ts}_tgenabled${tgenabled}_lower${lvl}_upper${upper}_affflex${VVV_AFF_FLEXIBLE}_mmap${mmap}_useprio${use_priority}_gemmtpb${gemm_tpb}_hwc${hwc_backend}"

                                        echo "  Running: $VVV_EXP_STR"

                                        run_experiment "$EXP_NSIZE" "$ts" "$imm" "$EXP_CPUS" \
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

    echo ""
    echo "All jobs submitted. Output directory: ${prefix_dir}/"
    echo "After jobs complete, run: python3 collect_results.py ${prefix_dir}"
}

main
