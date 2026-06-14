#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Launching all reproduction experiments ==="

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
