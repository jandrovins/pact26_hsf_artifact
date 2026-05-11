#!/usr/bin/env bash
set -euo pipefail

BINARY=${1:-./measure_work}
OUT=${2:-work_sweep.csv}

TARGET_P50_US=20000  # 20 ms

echo "niters,mean_us,std_us,min_us,p25_us,p50_us,p75_us,max_us" > "$OUT"

niters=100
while true; do
    echo "Running niters=$niters ..." >&2
    line=$(taskset -c 0 "$BINARY" "$niters")
    row=$(echo "$line" | awk '{
        for (i=1; i<=NF; i++) {
            split($i, kv, "=")
            val = kv[2]
            sub(/us$/, "", val)
            if      (kv[1] == "niters") niters = val
            else if (kv[1] == "mean")   mean   = val
            else if (kv[1] == "std")    std    = val
            else if (kv[1] == "min")    min    = val
            else if (kv[1] == "p25")    p25    = val
            else if (kv[1] == "p50")    p50    = val
            else if (kv[1] == "p75")    p75    = val
            else if (kv[1] == "max")    max    = val
        }
        print niters "," mean "," std "," min "," p25 "," p50 "," p75 "," max
    }')
    echo "$row" >> "$OUT"

    p50=$(echo "$line" | awk '{for(i=1;i<=NF;i++){split($i,kv,"="); if(kv[1]=="p50"){v=kv[2]; sub(/us$/,"",v); print v}}}')
    if awk "BEGIN{exit !($p50 >= $TARGET_P50_US)}"; then
        echo "Reached p50=${p50}us >= ${TARGET_P50_US}us, stopping." >&2
        break
    fi

    next=$(awk "BEGIN{printf \"%d\", $niters * 1.5}")
    if [ "$next" -le "$niters" ]; then
        next=$((niters + 1))
    fi
    niters=$next
done

echo "Saved to $OUT" >&2
