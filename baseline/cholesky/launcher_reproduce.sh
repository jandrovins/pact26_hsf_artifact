#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce external baseline Cholesky (libFLAME+BLIS)
#
# Pure LAPACKE_dpotrf via AOCL libFLAME + BLIS (no OmpSs-2 / nOS-V runtime).
# Configs taken from results/fox_cholesky_libflame/summary.csv.
# Results -> $REPO_ROOT/reproduced_results/fox_cholesky_libflame/
#
# NOTE: n=49152 is intentionally omitted. Its matrix has >2^31 elements and
# needs an ILP64 libFLAME; the Nix flake provides LP64 only. It is not plotted
# (cholesky figure panels use 6144 + 33792).
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NFS_REPO="$(echo "$REPO_ROOT" | sed 's|^/home/|/nfs/home/|')"

readonly BIN="cholesky_libflame.bin"
readonly NREPS=3
readonly PARTITION="${PARTITION:-fox}"
readonly RESULT_DIR="${REPO_ROOT}/reproduced_results/fox_cholesky_libflame"

# "N:CPUS"  (both numa 0 and 1 are run for each size)
readonly SIZES=(
    "6144:96"
    "33792:192"
)
readonly NUMAS=(0 1)

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:30:00"
}

main() {
    setup_slurm_env
    echo "=== Cholesky baseline (libFLAME+BLIS) ==="

    for sz in "${SIZES[@]}"; do
        IFS=':' read -r n cpus <<< "$sz"
        for numa in "${NUMAS[@]}"; do
            export VVV_NUMA_INTERLEAVED=$numa

            local exp_str="n${n}_numa${numa}"
            local raw_dir="${RESULT_DIR}/raw/${exp_str}"
            mkdir -p "$raw_dir"

            cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${exp_str}",
    "ppn": 1, "cpuspertask": $cpus, "nsize": $n, "numa": $numa
}
METAEOF
            local nfs_raw="$(echo "$raw_dir" | sed 's|^/home/|/nfs/home/|')"

            echo "  [cholesky] n=$n cpus=$cpus numa=$numa"
            sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
                --switches=1 --export=ALL \
                -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
                --output="${nfs_raw}/job%A_%a.out" \
                --error="${nfs_raw}/job%A_%a.err" \
                "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$n" "$cpus" "$numa"
        done
    done
    echo "All jobs submitted."
}

main
