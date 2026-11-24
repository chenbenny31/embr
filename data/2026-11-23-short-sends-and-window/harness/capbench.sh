#!/bin/bash
# capbench.sh <receiver cpu list> <runs> <config>...
# Packets per send against the egress path, loopback, 1 GiB, QUIC. Each config is
# "label|push ENV=value ...|pull ENV=value ...[|embr binary]". Configs are alternated; one warm-up transfer first.
# The sender is pinned to CPUs 6-9. Per transfer: the pull's wall time, user+sys of the push and of the
# pull (each its whole process), and the sender's egress line (datagrams, sends, copied, waits).
set -uo pipefail
S=${SCRATCH:?scratch directory holding the build, a 1 GiB file and a home with the QUIC cert}
default_embr=${EMBR:-$S/async8/build/embr}
file=${FILE:-$S/realperf/f1g.bin}
work=${WORK:-$S/async8/run}
mkdir -p "$work"; out=$work/out.bin
recv_cpus=$1 runs=$2; shift 2
configs=("$@")
port=10031
cert=(--cert "$S/home126/.config/embr/cert.pem" --key "$S/home126/.config/embr/key.pem")

one() { # <push env string> <pull env string> <binary> -> "wall push_user push_sys pull_user pull_sys datagrams sends copied waits exit"
    local -a push_env pull_env; read -r -a push_env <<< "$1"; read -r -a pull_env <<< "${2:-}"
    local embr=${3:-$default_embr}
    ( HOME=$S/home126 taskset -c 6-9 env EMBR_QUIC_STATS=1 "${push_env[@]}" /usr/bin/time -f "%U %S" -o "$work/push.time" \
        "$embr" push "$file" --port $port --transport quic "${cert[@]}" > /dev/null 2> "$work/push.err" & )
    until [[ -n "$(ss -H -lnu "sport = :$port" 2>/dev/null)" ]]; do sleep 0.05; done
    rm -f "$out" "$work/.out.bin.embr.partial"
    HOME=$S/home126 taskset -c "$recv_cpus" env "${pull_env[@]}" /usr/bin/time -f "%e %U %S %x" -o "$work/pull.time" \
        "$embr" pull 127.0.0.1 --port $port --transport quic --out "$out" > /dev/null 2> "$work/pull.err"
    local waited=0
    while pgrep -f "[e]mbr push .*--port $port" > /dev/null; do
        sleep 0.05; waited=$((waited + 1))
        (( waited == 100 )) && pkill -f "[e]mbr push .*--port $port" # still there only when the pull failed
    done
    local egress; egress=$(grep -a '^\[quic-egress\]' "$work/push.err" | sort -t= -k4 -n | tail -1)
    val() { echo "$egress" | grep -o " $1=[0-9]*" | cut -d= -f2; }
    echo "$(tail -1 "$work/pull.time" | awk '{print $1}') $(tail -1 "$work/push.time") $(tail -1 "$work/pull.time" | awk '{print $2, $3}')" \
         "$(val datagrams) $(val sends) $(val copied || true) $(val waits || true) $(tail -1 "$work/pull.time" | awk '{print $4}')" \
        | awk '{ if (NF == 8) print $1, $2, $3, $4, $5, $6, $7, "-", "-", $8; else print }'
}

IFS='|' read -r _ push0 pull0 bin0 <<< "${configs[0]}"; one "$push0" "${pull0:-}" "${bin0:-}" > /dev/null # warm up
for i in "${!configs[@]}"; do : > "$work/res.$i"; done
for (( r = 0; r < runs; r++ )); do
    for i in "${!configs[@]}"; do IFS='|' read -r _ push pull bin <<< "${configs[$i]}"; one "$push" "${pull:-}" "${bin:-}" >> "$work/res.$i"; done
done
echo "# loopback, $(stat -c %s "$file") bytes, receiver CPUs $recv_cpus, sender CPUs 6-9, kernel $(uname -r), n=$runs, medians"
for i in "${!configs[@]}"; do
    IFS='|' read -r label _ _ _ <<< "${configs[$i]}"
    awk -v label="$label" '
        $10 != 0 { bad++; next }
        { n++; wall[n] = $1; push[n] = $2 + $3; pushsys[n] = $3; pull[n] = $4 + $5; per[n] = ($7 > 0) ? $6 / $7 : 0; copied = $8; waits[n] = ($9 == "-") ? 0 : $9; sends[n] = $7 }
        function med(a,   i, j, tmp, b) { for (i = 1; i <= n; i++) b[i] = a[i]; for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (b[j] < b[i]) { tmp = b[i]; b[i] = b[j]; b[j] = tmp }
                                        return (n % 2) ? b[(n + 1) / 2] : (b[n / 2] + b[n / 2 + 1]) / 2 }
        function lo(a,   i, m) { m = a[1]; for (i = 2; i <= n; i++) if (a[i] < m) m = a[i]; return m }
        function hi(a,   i, m) { m = a[1]; for (i = 2; i <= n; i++) if (a[i] > m) m = a[i]; return m }
        END { if (n == 0) { printf "%-28s all %d runs failed\n", label, bad; exit }
              w = med(wall); printf "%-28s n=%d gbps=%.2f [%.2f, %.2f] packets_per_send=%.1f sends=%.0f push_cpu=%.2f (sys %.2f) push_cpu_of_wall=%.0f%% pull_cpu=%.2f waits=%.0f copied=%s failed=%d\n",
              label, n, 8.589934592 / w, 8.589934592 / hi(wall), 8.589934592 / lo(wall), med(per), med(sends), med(push), med(pushsys), 100 * med(push) / w, med(pull), med(waits), copied, bad + 0 }' "$work/res.$i"
done
