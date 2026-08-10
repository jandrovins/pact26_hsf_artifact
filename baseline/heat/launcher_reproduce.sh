#!/usr/bin/env bash
set -u
# =============================================================================
# launcher_reproduce.sh — Reproduce external baseline Heat (OpenMP)
#
# Pure OpenMP Gauss-Seidel heat solver (no OmpSs-2 / nOS-V runtime).
# Configs taken from results/fox_heat_omp/summary.csv.
# Results -> $REPO_ROOT/reproduced_results/fox_heat_omp/
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
readonly BIN="heat_omp.bin"
readonly NREPS="${NREPS:-3}"
readonly PARTITION="${PARTITION:-fox}"
readonly RESULT_DIR="${REPO_ROOT}/reproduced_results/fox_heat_omp"

# "N:BS:ITS:CPUS"  (each run for numa in {0,1} x procbind in {0,1})
readonly EXPERIMENTS=(
    "12288:768:800:96"
    "49152:512:60:192"
)
readonly NUMAS=(0 1)
readonly PROCBINDS=(0 1)

setup_slurm_env() {
    export SLURM_JOB_NUM_NODES=1
    export SBATCH_HINT=nomultithread
    export SLURM_HINT=$SBATCH_HINT
    export SBATCH_EXCLUSIVE=""
    export SBATCH_TIMELIMIT="00:30:00"
}

main() {
    setup_slurm_env
    echo "=== Heat baseline (OpenMP) ==="

    for exp in "${EXPERIMENTS[@]}"; do
        IFS=':' read -r n bs its cpus <<< "$exp"
        for numa in "${NUMAS[@]}"; do
            for procbind in "${PROCBINDS[@]}"; do
                export VVV_NUMA_INTERLEAVED=$numa
                export OMP_PROC_BIND=$procbind

                local exp_str="n${n}_b${bs}_t${its}_numa${numa}_procbind${procbind}"
                local raw_dir="${RESULT_DIR}/raw/${exp_str}"
                mkdir -p "$raw_dir"

                cat > "$raw_dir/exp_meta.json" <<METAEOF
{
    "experiment": "${exp_str}",
    "ppn": 1, "cpuspertask": $cpus, "n": $n, "bs": $bs, "its": $its,
    "numa": $numa, "procbind": $procbind
}
METAEOF
                local nfs_raw="$(nfsmap "$raw_dir")"
                echo "  [heat] n=$n bs=$bs its=$its numa=$numa procbind=$procbind"
                sbatch -p "$PARTITION" --chdir="$NFS_REPO" --array=1-${NREPS} \
                    --switches=1 --export=ALL \
                    -N 1 --ntasks-per-node=1 --cpus-per-task=$cpus \
                    --output="${nfs_raw}/job%A_%a.out" \
                    --error="${nfs_raw}/job%A_%a.err" \
                    "$SCRIPT_DIR/submit_reproduce.job" "$BIN" "$n" "$bs" "$its" "$cpus" "$numa" "$procbind"
            done
        done
    done
    echo "All jobs submitted."
}

main
