#!/bin/bash
# abench2.sh <quic|tcp> <receiver cpu list> <mask sha: 0|1> <runs> <config>...
# Loopback, 1 GiB. Each config is "label|pull ENV=value ...|prefill|push ENV=value ..." (prefill: 1 = the
# output file exists with all its pages before the pull). Configs are alternated. The sender is pinned to CPUs 6-9.
# Per pull: wall, process user+sys, CPU of the receiving thread (its own getrusage) and of the other threads.
set -uo pipefail
S=${SCRATCH:?scratch directory holding the build, a 1 GiB file and a home with the QUIC cert}
embr=${EMBR:-$S/async/build/embr}
file=$S/realperf/f1g.bin
out=${OUT:-$S/async/out.bin}
transport=$1 recv_cpus=$2 mask=$3 runs=$4; shift 4
configs=("$@")
port=10020; proto=t; push_env=()
if [[ $transport == quic ]]; then
    port=10021; proto=u; push_env=(EMBR_QUIC_GSO=1 EMBR_QUIC_EGRESS=sendmsg)
    push_tail=(--transport quic --cert "$S/home126/.config/embr/cert.pem" --key "$S/home126/.config/embr/key.pem")
else
    push_tail=(--transport tcp)
fi
mask_env=(); [[ $mask == 1 ]] && mask_env=(OPENSSL_ia32cap=:~0x20000000)

udp_drops() { awk '/^Udp:/ { if (!col) { for (i = 2; i <= NF; i++) if ($i == "RcvbufErrors") col = i } else { print $col; exit } }' /proc/net/snmp; }

one() { # <pull env string> <prefill> <push env string> -> "wall cpu drops 0 exit"
    local -a extra more; read -r -a extra <<< "$1"; read -r -a more <<< "${3:-}"
    local drops_before; drops_before=$(udp_drops)
    ( HOME=$S/home126 taskset -c 6-9 env "${push_env[@]}" "${more[@]}" "$embr" push "$file" --port $port "${push_tail[@]}" > /dev/null 2>&1 & )
    until [[ -n "$(ss -H -ln$proto "sport = :$port" 2>/dev/null)" ]]; do sleep 0.05; done
    rm -f "$out" "$(dirname "$out")/.$(basename "$out").embr.partial"
    [[ $2 == 1 ]] && fallocate -l "$(stat -c %s "$file")" "$out"
    HOME=$S/home126 taskset -c "$recv_cpus" env "${mask_env[@]}" "${extra[@]}" EMBR_EXPERIMENT_THREAD_CPU=1 \
        /usr/bin/time -f "%e %U %S %x" -o "$S/async/time.txt" \
        "$embr" pull 127.0.0.1 --port $port --transport "$transport" --out "$out" > /dev/null 2> "$S/async/pull2.err"
    # receiving thread from its own getrusage line; the other threads are the rest of the process
    awk -v t="$(tail -1 "$S/async/time.txt")" -v drops="$(( $(udp_drops) - drops_before ))" 'BEGIN { split(t, a, " ");
        printf "%s %.2f %.0f %.0f %s\n", a[1], a[2] + a[3], drops, 0, a[4] }'
    pkill -f "[e]mbr push .*--port $port" 2> /dev/null # only still there when the pull failed
    while pgrep -f "[e]mbr push .*--port $port" > /dev/null; do sleep 0.05; done
}

IFS='|' read -r _ env0 pre0 push0 <<< "${configs[0]}"; one "$env0" "${pre0:-0}" "${push0:-}" > /dev/null # warm up
for i in "${!configs[@]}"; do : > "$S/async/res2.$i"; done
for (( r = 0; r < runs; r++ )); do
    for i in "${!configs[@]}"; do IFS='|' read -r _ env pre push <<< "${configs[$i]}"; one "$env" "${pre:-0}" "${push:-}" >> "$S/async/res2.$i"; done
done
for i in "${!configs[@]}"; do
    IFS='|' read -r label _ _ _ <<< "${configs[$i]}"
    awk -v label="$label" -v t="$transport" '
        $5 != 0 { bad++; next }
        { n++; wall[n] = $1; cpu[n] = $2; first[n] = $3; rest[n] = $4 }
        function med(a,   i, j, tmp, b) { for (i = 1; i <= n; i++) b[i] = a[i]; for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (b[j] < b[i]) { tmp = b[i]; b[i] = b[j]; b[j] = tmp }
                                        return (n % 2) ? b[(n + 1) / 2] : (b[n / 2] + b[n / 2 + 1]) / 2 }
        END { w = med(wall); printf "%-40s %-4s n=%d gbps=%.2f wall=%.2f process_cpu=%.2f udp_drops=%.0f failed=%d\n",
              label, t, n, 8.589934592 / w, w, med(cpu), med(first), bad + 0 }' "$S/async/res2.$i"
done
