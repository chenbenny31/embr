#!/bin/bash
# prehash.sh <sender cpu list> <mask sha: 0|1> [runs]
# The sender's first push of a file: seconds from starting `embr push` until its port is bound, which
# is the time to hash 1 GiB into .embr.hash (the cache is removed before every run).
# Compares EMBR_HASH_LANES=1 (chunk by chunk) with the default (side by side where the CPU allows).
set -uo pipefail
S=${SCRATCH:?scratch directory holding the build, a 1 GiB file and a home with the QUIC cert}
embr=${EMBR:-$S/build-126-Release/embr}
file=$S/realperf/f1g.bin
cache=$S/realperf/.f1g.bin.embr.hash
cpus=$1 mask=$2 runs=${3:-5}
mask_env=(); [[ $mask == 1 ]] && mask_env=(OPENSSL_ia32cap=:~0x20000000)
port=10020

one() { # <extra env or empty> -> seconds until the port is bound
    rm -f "$cache"
    local start end
    start=$(date +%s.%N)
    ( HOME=$S/home126 taskset -c "$cpus" env "${mask_env[@]}" $1 "$embr" push "$file" --port $port --transport tcp > /dev/null 2>&1 & )
    until [[ -n "$(ss -H -lnt "sport = :$port" 2>/dev/null)" ]]; do sleep 0.005; done
    end=$(date +%s.%N)
    pkill -f "[e]mbr push .*--port $port"
    while pgrep -f "[e]mbr push .*--port $port" > /dev/null; do sleep 0.02; done
    echo "$end - $start" | bc -l
}

one "XX=1" > /dev/null
for label in "chunk by chunk|EMBR_HASH_LANES=1" "side by side|XX=1"; do
    IFS='|' read -r name env <<< "$label"
    for (( i = 0; i < runs; i++ )); do one "$env"; done | sort -n | awk -v name="$name" -v cpus="$cpus" -v mask="$mask" '
        { v[NR] = $1 } END { m = (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2;
            printf "sender CPUs %-10s %-10s %-15s n=%d seconds=%.3f [%.3f, %.3f]  %.0f MB/s\n", cpus, (mask == 1 ? "no sha ext" : "sha ext"), name, NR, m, v[1], v[NR], 1073.741824 / m }'
done
