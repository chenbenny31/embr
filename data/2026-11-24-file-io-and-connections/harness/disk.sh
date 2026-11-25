#!/bin/bash
# disk.sh <runs> <case>...
# Does the file end limit a QUIC transfer? Loopback, defaults, one transfer per run.
# Each case is "label|source file|output file|cold": cold = 1 drops the source file's pages from the
# page cache after the push has hashed it and is listening, so the push reads it from the disk while sending.
# Per transfer: the pull's wall time, user+sys of push and pull (whole process), and how much of the
# source was in the page cache just before the pull started.
set -uo pipefail
S=${SCRATCH:?scratch directory holding the build and a home with the QUIC cert}
embr=${EMBR:-$S/resume/build-tree/embr}
work=${WORK:-$S/asyncio/run}; mkdir -p "$work"
runs=$1; shift
port=10033
cert=(--cert "$S/home126/.config/embr/cert.pem" --key "$S/home126/.config/embr/key.pem")

one() { # <source> <output> <cold> -> "wall push_cpu pull_cpu cached_pct exit"
    local src=$1 out=$2 cold=$3
    ( HOME=$S/home126 taskset -c 6-9 /usr/bin/time -f "%U %S" -o "$work/push.time" \
        "$embr" push "$src" --port $port --transport quic "${cert[@]}" > /dev/null 2> "$work/push.err" & )
    local tries=0
    until [[ -n "$(ss -H -lnu "sport = :$port" 2>/dev/null)" ]]; do
        sleep 0.05; tries=$((tries + 1)); (( tries > 1200 )) && { echo "push never listened" >&2; exit 1; }
    done
    [[ $cold == 1 ]] && dd if="$src" iflag=nocache count=0 status=none
    local cached; cached=$(fincore -n -b -o PAGES "$src" | awk -v size="$(stat -c %s "$src")" '{ printf "%.0f", 100 * $1 * 4096 / size }')
    rm -f "$out" "$(dirname "$out")/.$(basename "$out").embr.partial"
    HOME=$S/home126 taskset -c 2-5 /usr/bin/time -f "%e %U %S %x" -o "$work/pull.time" \
        "$embr" pull 127.0.0.1 --port $port --transport quic --out "$out" > /dev/null 2> "$work/pull.err"
    local waited=0
    while [[ -n "$(ss -H -lnu "sport = :$port" 2>/dev/null)" ]]; do
        sleep 0.05; waited=$((waited + 1)); (( waited > 200 )) && break
    done
    echo "$(awk 'END { print $1 }' "$work/pull.time") $(awk 'END { print $1 + $2 }' "$work/push.time")" \
         "$(awk 'END { print $2 + $3 }' "$work/pull.time") $cached $(awk 'END { print $4 }' "$work/pull.time")"
    rm -f "$out"
}

echo "# loopback QUIC, defaults, kernel $(uname -r), n=$runs, medians [min, max]; sender CPUs 6-9, receiver CPUs 2-5"
for c in "$@"; do
    IFS='|' read -r label src out cold <<< "$c"
    size=$(stat -c %s "$src")
    : > "$work/res"
    for (( r = 0; r < runs; r++ )); do one "$src" "$out" "$cold" >> "$work/res"; done
    awk -v label="$label" -v size="$size" '
        $5 != 0 { bad++; next }
        { n++; wall[n] = $1; push[n] = $2; pull[n] = $3; cached = $4 }
        function med(a,   i, j, tmp, b) { for (i = 1; i <= n; i++) b[i] = a[i]; for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (b[j] < b[i]) { tmp = b[i]; b[i] = b[j]; b[j] = tmp }
                                        return (n % 2) ? b[(n + 1) / 2] : (b[n / 2] + b[n / 2 + 1]) / 2 }
        function lo(a,   i, m) { m = a[1]; for (i = 2; i <= n; i++) if (a[i] < m) m = a[i]; return m }
        function hi(a,   i, m) { m = a[1]; for (i = 2; i <= n; i++) if (a[i] > m) m = a[i]; return m }
        END { if (n == 0) { printf "%-34s all %d runs failed\n", label, bad; exit }
              g = size * 8 / 1e9; gib = size / 1073741824
              printf "%-34s n=%d gbps=%.2f [%.2f, %.2f] wall=%.2f push_cpu_per_gib=%.2f pull_cpu_per_gib=%.2f source_cached=%s%% failed=%d\n",
              label, n, g / med(wall), g / hi(wall), g / lo(wall), med(wall), med(push) / gib, med(pull) / gib, cached, bad + 0 }' "$work/res"
done
