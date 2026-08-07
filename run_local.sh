#!/usr/bin/env bash
set -euo pipefail
# =============================================================================
# run_local.sh — SLURM-free "getting started" smoke test (~30 min incl. build).
#
# Builds and runs ONE small Cholesky configuration for both the unmodified
# OmpSs-2 baseline (nosvorig) and HSF (hsf), directly on the local cores via
# taskset (no sbatch/srun). This exercises the whole software stack — the
# OmpSs-2 compiler (clangOmpss2), nOS-V, NODES, tglib and BLIS/libFLAME — and
# demonstrates HSF's taskgroup scheduling versus the baseline.
#
# It is a FUNCTIONAL check: it proves the artifact builds and runs. Absolute
# performance is only meaningful on the paper's AMD EPYC 9684X (Genoa-X); see
# README / ARTIFACT_APPENDIX for the full, SLURM-based reproduction.
#
# Configurable via environment (all optional):
#   N        matrix size            (default: 6144)
#   TS       tile/block size        (default: 256)
#   CPUS     cores to run on        (default: min(nproc, 48))
#   HSF_ARCH build target arch      (default: native; set znver4 on Genoa-X)
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
N="${N:-6144}"
TS="${TS:-256}"
NPROC="$(nproc 2>/dev/null || echo 8)"
CPUS="${CPUS:-$(( NPROC < 48 ? NPROC : 48 ))}"
export HSF_ARCH="${HSF_ARCH:-native}"

echo "=== HSF local smoke test ==="
echo "    N=$N  TS=$TS  CPUS=$CPUS  HSF_ARCH=$HSF_ARCH"
echo "    (first run builds the toolchain from source and may take a while)"
echo ""

# run_one <flake_dir> <label> <extra "KEY=VALUE" env pairs...>
run_one() {
    local flake="$1"; shift
    local label="$1"; shift
    local bench_dir="${SCRIPT_DIR}/${flake}/cholesky"
    echo "----------------------------------------------------------------------"
    echo ">>> ${label}  (${flake}/cholesky)"
    echo "----------------------------------------------------------------------"
    # Common runtime config; topology.binding=inherit makes nOS-V honor the
    # taskset CPU mask instead of a fixed SLURM binding.
    local common_env=(
        "NOSV_CONFIG=nosv.toml"
        "VVV_MMAP_ENABLED=1"
        "VVV_EXP_STR=local_N${N}_TS${TS}"
    )
    env "${common_env[@]}" "$@" \
        nix develop --impure "${SCRIPT_DIR}/${flake}" --command bash -c "
            set -e
            cd '${bench_dir}'
            echo '[build] make cholesky_oss.bin'
            make cholesky_oss.bin >/dev/null
            echo '[run ] taskset -c 0-$((CPUS-1)) ./cholesky_oss.bin ${N} ${TS} 1'
            taskset -c 0-$((CPUS-1)) ./cholesky_oss.bin ${N} ${TS} 1
        "
    echo ""
}

# 1) Unmodified OmpSs-2 baseline (no taskgroups).
run_one nosvorig "OmpSs-2 baseline" \
    "VVV_TG_ENABLED=0" \
    "VVV_NUMA_INTERLEAVED=0" \
    "NOSV_CONFIG_OVERRIDE=topology.binding=inherit,monitoring.enabled=false,hwcounters.backend=none,scheduler.immediate_successor=true"

# 2) HSF taskgroups (per-column CS/NODE affinity, priority scheduling).
run_one hsf "HSF (taskgroups)" \
    "VVV_TG_ENABLED=1" "VVV_LOWER_LVL=cs" "VVV_UPPER_LVL=node" "VVV_AFF_FLEXIBLE=1" \
    "VVV_PRIORITY_ENABLED=1" "VVV_CHOL_GEMM_TILES_PER_BLOCK=48" \
    "NOSV_CONFIG_OVERRIDE=topology.binding=inherit,hwcounters.backend=none,scheduler.immediate_successor=false,taskgroups.save_hierarchy=false"

echo "======================================================================"
echo "Smoke test complete. Both runtimes built and ran the Cholesky kernel."
echo "Look at the two 'Printing result' / timing lines above (GFLOP/s)."
echo "For the full paper reproduction use launch_all.sh on a SLURM cluster."
echo "======================================================================"
