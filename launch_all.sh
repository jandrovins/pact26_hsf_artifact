#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# launch_all.sh — submit every reproduction experiment.
#
# Configurable via environment variables (all optional):
#   PARTITION   SLURM partition to submit to           (default: fox)
#   NREPS       repetitions per benchmark config        (default: 3)
#   NRUNS       repetitions for the oss_overhead sweep  (default: 5)
#   HSF_ARCH    target CPU arch for the build           (default: native;
#               set to znver4 for the paper's AMD EPYC 9684X / Genoa-X)
#   ACCOUNT     SLURM account   (optional; passed via SBATCH_ACCOUNT)
#   QOS         SLURM QoS       (optional; passed via SBATCH_QOS)
#   TIMELIMIT   SLURM time limit override (optional; passed via SBATCH_TIMELIMIT)
#
# Example (faithful reproduction on Genoa-X):
#   HSF_ARCH=znver4 NREPS=3 bash launch_all.sh
# Example (functional test elsewhere, fewer reps):
#   PARTITION=gpu NREPS=1 bash launch_all.sh
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Export tunables so the per-benchmark launchers (which use ${VAR:-default})
# and the SLURM jobs (submitted with --export=ALL) inherit them.
export PARTITION="${PARTITION:-fox}"
export NREPS="${NREPS:-3}"
export NRUNS="${NRUNS:-5}"
export HSF_ARCH="${HSF_ARCH:-native}"
# sbatch natively honors these env vars; only export when the user set them.
[ -n "${ACCOUNT:-}" ]   && export SBATCH_ACCOUNT="$ACCOUNT"
[ -n "${QOS:-}" ]       && export SBATCH_QOS="$QOS"
[ -n "${TIMELIMIT:-}" ] && export SBATCH_TIMELIMIT="$TIMELIMIT"

echo "=== Launching all reproduction experiments ==="
echo "    PARTITION=$PARTITION  NREPS=$NREPS  NRUNS=$NRUNS  HSF_ARCH=$HSF_ARCH" \
     "${ACCOUNT:+ACCOUNT=$ACCOUNT}" "${QOS:+QOS=$QOS}"

for launcher in \
    nosvorig/cholesky/launcher_reproduce.sh \
    nosvorig/heat/launcher_reproduce.sh \
    nosvorig/hpccg/launcher_reproduce.sh \
    nosvorig/matmul/launcher_reproduce.sh \
    nosvorig/multisaxpy/launcher_reproduce.sh \
    hsf/cholesky/launcher_reproduce.sh \
    hsf/heat/launcher_reproduce.sh \
    hsf/hpccg/launcher_reproduce.sh \
    hsf/matmul/launcher_reproduce.sh \
    hsf/multisaxpy/launcher_reproduce.sh \
    nosvorig/oss_overhead/launcher_reproduce.sh \
    hsf/oss_overhead/launcher_reproduce.sh \
    baseline/cholesky/launcher_reproduce.sh \
    baseline/heat/launcher_reproduce.sh \
    baseline/hpccg/launcher_reproduce.sh \
    baseline/matmul/launcher_reproduce.sh \
    baseline/multisaxpy/launcher_reproduce.sh \
; do
    echo ""
    echo "--- Running $launcher ---"
    bash "$SCRIPT_DIR/$launcher"
done

echo ""
echo "=== All launchers completed successfully ==="
