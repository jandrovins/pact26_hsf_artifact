#!/usr/bin/env bash
set -u

# ============================================================================
# Matmul Launcher — 2D Superblock Taskgroup Experiments
# ============================================================================
# Adapted from the Cholesky launcher_new.sh style for the matmul benchmark.
# Sweeps over 2D taskgroup hierarchy modes (flat vs two-level), affinity
# levels, tile sizes, and other relevant parameters.
#
# Usage:
#   ./launcher_new.sh <outdir_label> <binary>
#
# Example:
#   ./launcher_new.sh matmul_2d_tg 02.matmul_ompss2_itampi.bin
# ============================================================================

# ============================================================================
# CONFIGURATION SECTION
# ============================================================================

readonly OUTDIR_BASE=$1
readonly BIN=$2

readonly NREPS=30
readonly OVNI=none

# Slurm partition — controls which cluster to submit to and sets the L3 size
# used for superblock sizing.  Override via environment: PARTITION=owl ./launcher_new.sh ...
readonly PARTITION="${PARTITION:-fox}"

# Derive per-domain L3 size from the partition
if [ "$PARTITION" = "owl" ]; then
    readonly VVV_L3_SIZE_MIB_DEFAULT=35
elif [ "$PARTITION" = "fox" ]; then
    readonly VVV_L3_SIZE_MIB_DEFAULT=96
else
    readonly VVV_L3_SIZE_MIB_DEFAULT=96
    echo "Warning: unknown partition '${PARTITION}', defaulting L3 to 96 MiB"
fi
export VVV_L3_SIZE_MIB=${VVV_L3_SIZE_MIB:-$VVV_L3_SIZE_MIB_DEFAULT}

# Map lower -> possible upper levels for affinity sweep
declare -A UPPER_MAP=(
    [node]="node"
    [numa]="numa node"
    [cs]="cs numa"
    [core]="core"
)

# Experiments: "N:M:TILESIZE_LIST:TIMESTEPS:WARMUP:CPUS:PPN"
readonly EXPERIMENTS=(
    "6144:6144:64,96:100:1:96:1"
    #"8192:8192:192,256,512,1024:10:1:24:8"
    #"8192:8192:128,192,256,512:10:1:24:8"
    #"16384:16384:128,192,256,512:50:1:24:8"
    #"49152:49152:1024:1:0:192:1"
    #"65536:65536:128,192,256,512:3:1:24:8"
)

# Affinity levels to sweep
readonly LEVELS=("cs" "numa" "node")
readonly IMM_VALUES=("false")

# TG enabled: 0 = disabled (baseline), 1 = enabled (2D blocking)
readonly TG_ENABLED_VALUES=(1)

# Memory mapping
readonly MMAP_VALUES=(1)

# --- Matmul-specific 2D taskgroup parameters ---
# VVV_MATMUL_TG_HIERARCHY: 0 = flat grid, 1 = two-level (region -> block)
readonly TG_HIERARCHY_VALUES=(0)

# HW counter backend
readonly HWC_BACKENDS=("none")

# VVV_FORCE_NBLOCKS: 0 = L3-optimal sizing, 1 = force num_blocks == num_domains
readonly FORCE_NBLOCKS_VALUES=(1)

# Source patterns to snapshot into the output prefix
readonly SRC_PATTERNS=("*.c" "*.h" "Makefile" "*.toml" "*.sh" "*.job" "*.md")

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
    # Also snapshot the actual source tree
    if [ -d "src" ]; then
        cp -r src "${prefix_dir}/src/src_tree" 2>/dev/null || true
    fi
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
        export SBATCH_TIMELIMIT="00:10:00"
    fi
}

setup_nosv_env() {
    local hwc_backend=$1
    # Use a partition-specific nOS-V config when available.
    # If running on the 'owl' partition, prefer nosv-owl.toml; otherwise use the default nosv.toml.
    if [ "$PARTITION" = "owl" ]; then
        export NOSV_CONFIG=nosv-owl.toml
    else
        export NOSV_CONFIG=nosv.toml
    fi
    export NOSV_CONFIG_OVERRIDE="topology.binding=inherit"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.enabled=false"
    export NOSV_CONFIG_OVERRIDE+=",monitoring.verbose=false"
    export NOSV_CONFIG_OVERRIDE+=",hwcounters.backend=${hwc_backend}"
    export NOSV_CONFIG_OVERRIDE+=",taskgroups.save_hierarchy=false"
    export NOSV_CONFIG_OVERRIDE+=",taskgroups.dot_filename=tg_hierarchy.dot"
}

parse_experiment() {
    local exp=$1
    IFS=':' read -r EXP_N EXP_M EXP_TS_STR EXP_TIMESTEPS EXP_WARMUP EXP_CPUS EXP_PPN <<< "$exp"
    IFS=',' read -ra EXP_TS_ARR <<< "$EXP_TS_STR"
}

run_experiment() {
    local nsize=$1 msize=$2 ts=$3 imm=$4 ppn=$5
    local timesteps=$6 warmup=$7
    local srunout=$8
    mkdir -p "$srunout"

    export SLURM_JOB_NAME="${OUTDIR_BASE}_${VVV_EXP_STR}"
    export SBATCH_JOB_NAME="${OUTDIR_BASE}_${VVV_EXP_STR}"
    export SBATCH_OUTPUT="${srunout}/job%J.out"
    export SBATCH_ERROR="${srunout}/job%J.err"
    export SRUN_OUTPUT="$SBATCH_OUTPUT"
    export SRUN_ERROR="$SBATCH_ERROR"

    unset SLURM_JOB_ID

    local max_retries=100
    local retry_count=0
    while [ $retry_count -lt $max_retries ]; do
        sbatch -p "$PARTITION" --array=1-${NREPS} \
            --switches=1 \
            --export=ALL \
            -N "$SLURM_JOB_NUM_NODES" \
            --ntasks-per-node="$SLURM_NTASKS_PER_NODE" \
            --cpus-per-task="$SRUN_CPUS_PER_TASK" \
            ./submit-insalloc-new.job "$BIN" "$nsize" "$msize" "$ts" \
                "$timesteps" "$warmup" "$imm" "$OVNI" && break
        retry_count=$((retry_count + 1))
        echo "sbatch failed, retry $retry_count/$max_retries in 30 seconds..."
        sleep 30
    done
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

                for tgenabled in "${TG_ENABLED_VALUES[@]}"; do
                    export VVV_TG_ENABLED=$tgenabled

                    for tg_hier in "${TG_HIERARCHY_VALUES[@]}"; do
                        export VVV_MATMUL_TG_HIERARCHY=$tg_hier

                        # Skip hierarchy sweep when TGs are disabled (only run hier=0 once)
                        if [ "$tgenabled" -eq 0 ] && [ "$tg_hier" -ne 0 ]; then
                            continue
                        fi

                        for imm in "${IMM_VALUES[@]}"; do
                        for mmap in "${MMAP_VALUES[@]}"; do
                        for forcenb in "${FORCE_NBLOCKS_VALUES[@]}"; do
                            export VVV_MMAP_ENABLED=$mmap
                            export VVV_FORCE_NBLOCKS=$forcenb

                            for exp in "${EXPERIMENTS[@]}"; do
                                parse_experiment "$exp"

                                for ts in "${EXP_TS_ARR[@]}"; do
                                    export SLURM_NTASKS_PER_NODE=$EXP_PPN
                                    export SLURM_NPROCS=$EXP_PPN
                                    export SRUN_CPUS_PER_TASK=$EXP_CPUS

                                    export VVV_EXP_STR="imm${imm}_ppn${EXP_PPN}_n${EXP_N}_m${EXP_M}_ts${ts}_tge${tgenabled}_hier${tg_hier}_lower${lvl}_upper${upper}_affflex${VVV_AFF_FLEXIBLE}_mmap${mmap}_forcenb${forcenb}_hwc${hwc_backend}_l3${VVV_L3_SIZE_MIB}m"

                                    echo "  Running: $VVV_EXP_STR"

                                    run_experiment "$EXP_N" "$EXP_M" "$ts" "$imm" "$EXP_PPN" \
                                        "$EXP_TIMESTEPS" "$EXP_WARMUP" \
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
    done

    echo ""
    echo "All jobs submitted. Output directory: ${prefix_dir}/"
}

main
