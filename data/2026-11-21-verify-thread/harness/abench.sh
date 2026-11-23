#!/bin/bash
# abench.sh <label> <quic|tcp> <receiver cpu list> <mask sha: 0|1> [runs]
# Loopback, 1 GiB: the same binary with EMBR_VERIFY=inline (hash on the receiving thread, as today)
# and EMBR_VERIFY=thread (hash on a second thread), runs alternated. The sender is pinned to CPUs 6-9.
# mask sha = 1 hides the CPU's SHA extensions from the receiver's OpenSSL, as on a CPU without them.
set -uo pipefail
S=${SCRATCH:?scratch directory holding the build, a 1 GiB file and a home with the QUIC cert}
embr=$S/async/build/embr
file=$S/realperf/f1g.bin
out=$S/async/out.bin
label=$1 transport=$2 recv_cpus=$3 mask=$4 runs=${5:-7}
port=10020; proto=t; push_env=()
if [[ $transport == quic ]]; then
    port=10021; proto=u; push_env=(EMBR_QUIC_GSO=1 EMBR_QUIC_EGRESS=sendmsg)
    push_tail=(--transport quic --cert "$S/home126/.config/embr/cert.pem" --key "$S/home126/.config/embr/key.pem")
else
    push_tail=(--transport tcp)
fi
recv_env=(); [[ $mask == 1 ]] && recv_env=(OPENSSL_ia32cap=:~0x20000000)

one() { # <mode> -> "wall user sys" of the pull
    local mode=$1
    ( HOME=$S/home126 taskset -c 6-9 env "${push_env[@]}" "$embr" push "$file" --port $port "${push_tail[@]}" > /dev/null 2> "$S/async/push.err" & )
    until [[ -n "$(ss -H -ln$proto "sport = :$port" 2>/dev/null)" ]]; do sleep 0.05; done
    rm -f "$out" "$S/async/.out.bin.embr.partial"
    HOME=$S/home126 taskset -c "$recv_cpus" env "${recv_env[@]}" EMBR_VERIFY="$mode" \
        /usr/bin/time -f "%e %U %S %x" -o "$S/async/time.txt" "$embr" pull 127.0.0.1 --port $port --transport "$transport" --out "$out" > /dev/null 2> "$S/async/pull.err"
    cat "$S/async/time.txt"
    while pgrep -f "[e]mbr push .*--port $port" > /dev/null; do sleep 0.05; done
}

one inline > /dev/null # warm the page cache and the hash cache
for mode in inline thread; do : > "$S/async/res.$mode"; done
for (( i = 0; i < runs; i++ )); do
    for mode in inline thread; do one "$mode" >> "$S/async/res.$mode"; done
done
for mode in inline thread; do
    awk -v label="$label" -v mode="$mode" -v t="$transport" '
        $4 != 0 { bad++ ; next }
        { n++; wall[n] = $1; user[n] = $2; sys[n] = $3; cpu[n] = $2 + $3 }
        function med(a,   i, j, tmp, b) { for (i = 1; i <= n; i++) b[i] = a[i]; for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (b[j] < b[i]) { tmp = b[i]; b[i] = b[j]; b[j] = tmp }
                                        return (n % 2) ? b[(n + 1) / 2] : (b[n / 2] + b[n / 2 + 1]) / 2 }
        function lo(a,   i, m) { m = a[1]; for (i = 2; i <= n; i++) if (a[i] < m) m = a[i]; return m }
        function hi(a,   i, m) { m = a[1]; for (i = 2; i <= n; i++) if (a[i] > m) m = a[i]; return m }
        END { w = med(wall); printf "%-34s %-5s %-7s n=%d gbps=%.2f [%.2f, %.2f] wall=%.2f user=%.2f sys=%.2f cpu=%.2f failed=%d\n",
              label, t, mode, n, 8.589934592 / w, 8.589934592 / hi(wall), 8.589934592 / lo(wall), w, med(user), med(sys), med(cpu), bad + 0 }' "$S/async/res.$mode"
done
