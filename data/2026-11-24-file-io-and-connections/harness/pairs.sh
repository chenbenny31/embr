#!/bin/bash
# pairs.sh <runs> <k>...
# What would k connections give? k independent QUIC transfers of the same 1 GiB file at once on loopback,
# each push on its own CPU and each pull on its own two CPUs (receiving thread + verifier).
# Total rate = k GiB over the time from the first pull's start to the last pull's end.
set -uo pipefail
S=${SCRATCH:?scratch directory holding the build and a home with the QUIC cert}
embr=${EMBR:-$S/resume/build-tree/embr}
file=${FILE:?the 1 GiB source file}
work=${WORK:-$S/asyncio/pairs}; mkdir -p "$work"
runs=$1; shift
cert=(--cert "$S/home126/.config/embr/cert.pem" --key "$S/home126/.config/embr/key.pem")
send_cpu=(6 7 8 9); recv_cpus=(0,1 2,3 4,5 10,11)

round() { # <k> -> "wall failed"
    local k=$1 i
    for (( i = 0; i < k; i++ )); do
        ( HOME=$S/home126 taskset -c "${send_cpu[$i]}" "$embr" push "$file" --port $((10040 + i)) --transport quic "${cert[@]}" > /dev/null 2> "$work/push.$i.err" & )
    done
    for (( i = 0; i < k; i++ )); do
        local tries=0
        until [[ -n "$(ss -H -lnu "sport = :$((10040 + i))" 2>/dev/null)" ]]; do
            sleep 0.05; tries=$((tries + 1)); (( tries > 600 )) && { echo "push $i never listened" >&2; exit 1; }
        done
        rm -f "$work/out.$i" "$work/.out.$i.embr.partial"
    done
    local start; start=$(date +%s.%N)
    local -a pids=()
    for (( i = 0; i < k; i++ )); do
        HOME=$S/home126 taskset -c "${recv_cpus[$i]}" "$embr" pull 127.0.0.1 --port $((10040 + i)) --transport quic --out "$work/out.$i" > /dev/null 2> "$work/pull.$i.err" &
        pids+=($!)
    done
    local failed=0
    for p in "${pids[@]}"; do wait "$p" || failed=$((failed + 1)); done
    local end; end=$(date +%s.%N)
    for (( i = 0; i < k; i++ )); do
        local waited=0
        while [[ -n "$(ss -H -lnu "sport = :$((10040 + i))" 2>/dev/null)" ]]; do sleep 0.05; waited=$((waited + 1)); (( waited > 200 )) && break; done
        rm -f "$work/out.$i"
    done
    echo "$(echo "$end - $start" | bc -l) $failed"
}

round 1 > /dev/null # warm up
echo "# loopback QUIC, defaults, $(stat -c %s "$file") bytes per transfer, kernel $(uname -r), n=$runs, medians [min, max]"
for k in "$@"; do
    : > "$work/res"
    for (( r = 0; r < runs; r++ )); do round "$k" >> "$work/res"; done
    awk -v k="$k" -v size="$(stat -c %s "$file")" '
        $2 != 0 { bad++; next }
        { n++; wall[n] = $1 }
        function med(a,   i, j, tmp, b) { for (i = 1; i <= n; i++) b[i] = a[i]; for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (b[j] < b[i]) { tmp = b[i]; b[i] = b[j]; b[j] = tmp }
                                        return (n % 2) ? b[(n + 1) / 2] : (b[n / 2] + b[n / 2 + 1]) / 2 }
        function lo(a,   i, m) { m = a[1]; for (i = 2; i <= n; i++) if (a[i] < m) m = a[i]; return m }
        function hi(a,   i, m) { m = a[1]; for (i = 2; i <= n; i++) if (a[i] > m) m = a[i]; return m }
        END { if (n == 0) { printf "%d at once: all %d rounds failed\n", k, bad; exit }
              g = k * size * 8 / 1e9
              printf "%d at once   n=%d total_gbps=%.2f [%.2f, %.2f] wall=%.2f failed_rounds=%d\n", k, n, g / med(wall), g / hi(wall), g / lo(wall), med(wall), bad + 0 }' "$work/res"
done
