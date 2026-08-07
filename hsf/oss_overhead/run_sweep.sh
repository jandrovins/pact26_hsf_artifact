#!/usr/bin/env bash
set -euo pipefail

NTASKS=307200
NRUNS=5
NITERS_LIST=(5000 8500 14450 24565 41761 70994 120690 205173 348795 592952 1008019 1713633 2913177 4952401)
#NITERS_LIST=(5000 8500 14450)
#RUNTIMES=(oss omp oss_taskloop omp_taskloop)
RUNTIMES=(oss_taskloop oss)

NCORES=192

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RAWDIR="${SCRIPT_DIR}/results/${TIMESTAMP}_tmp"
mkdir -p "${SCRIPT_DIR}/results" "$RAWDIR"

export NOSV_CONFIG="${SCRIPT_DIR}/nosv.toml"
echo $NOSV_CONFIG
export OMP_PROC_BIND=true

HEADER="run,wall_s,ntasks,niters,mean_us,std_us,min_us,p25_us,p50_us,p75_us,max_us"

for runtime in "${RUNTIMES[@]}"; do
    binary="${SCRIPT_DIR}/overhead_${runtime}"
    if [[ ! -x "$binary" ]]; then
        echo "WARNING: binary not found or not executable: $binary — skipping" >&2
        continue
    fi

    for niters in "${NITERS_LIST[@]}"; do
        outfile="${SCRIPT_DIR}/results/${runtime}_ntasks${NTASKS}_niters${niters}_${TIMESTAMP}.csv"
        echo "$HEADER" > "$outfile"

        for ((run=1; run<=NRUNS; run++)); do
            echo "[${runtime}] ntasks=${NTASKS} niters=${niters} run=${run}/${NRUNS} → ${outfile}" >&2

            rawfile="${RAWDIR}/${runtime}_ntasks${NTASKS}_niters${niters}_run${run}.csv"
            env_prefix="OMP_NUM_THREADS=${NCORES}"
            env $env_prefix VVV_OVERHEAD_DATA="$rawfile" taskset -c 0-$((NCORES-1)) "$binary" "$NTASKS" "$niters" | awk -v r="$run" '
                /wall_s=/ { for (i=1;i<=NF;i++) { split($i,kv,"="); v[kv[1]]=kv[2] } }
                /mean=/   { for (i=1;i<=NF;i++) { split($i,kv,"="); gsub(/us$/,"",kv[2]); v[kv[1]]=kv[2] }
                             print r","v["wall_s"]","v["ntasks"]","v["niters"]","v["mean"]","v["std"]","v["min"]","v["p25"]","v["p50"]","v["p75"]","v["max"]
                           }
            ' >> "$outfile"
        done

    done
done
