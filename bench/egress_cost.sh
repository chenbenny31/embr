#!/usr/bin/env bash
# egress_cost.sh — what one send costs on each QUIC egress path, with no QUIC and no receiver.
# Run from the repo root:
#
#   bash bench/egress_cost.sh                          the device frees each packet at transmit
#   DELAY=100us bash bench/egress_cost.sh              the device holds each packet that long first
#   DELAY=100us SLOTS=128 bash bench/egress_cost.sh    same, with a larger SEND_ZC slot pool
#
# No root needed: it runs in a private user + network namespace on a dummy device. A dummy
# device frees every packet at transmit, so SEND_ZC is never copied and its completion arrives
# inside the send call: the best case SEND_ZC can have. DELAY puts a netem qdisc in front,
# which holds packets, and with them the SEND_ZC completions, as a NIC's transmit queue does.
#
# A row sends that many packets per send: 1 is plain datagrams, two per flush as embr does at
# the default ACK rate; more is one UDP GSO send per flush. Columns are nanoseconds per send,
# median of REPS runs. CPU is user + sys + softirq of the pinned core; wall above CPU means
# the sender slept waiting for a free SEND_ZC slot, and "waits" counts those sleeps.
#
# Knobs: PKT SEGS_LIST REPS CPU DELAY SLOTS BIN
# Needs liburing, unshare, ip, and for DELAY tc with sch_netem (Fedora: kernel-modules-extra).

set -euo pipefail

PKT="${PKT:-1350}"
SEGS_LIST="${SEGS_LIST:-1 2 4 8 10 12 14 16 20 24 32 47}"
REPS="${REPS:-5}"
CPU="${CPU:-$(( $(nproc) - 1 ))}"
DELAY="${DELAY:-}"
SLOTS="${SLOTS:-}" # empty: what QuicTransport uses, 64 or 8 with GSO
BIN="${BIN:-/tmp/embr-egress-cost}"

if [[ -z "${EGRESS_COST_IN_NS:-}" ]]; then
    [[ -f bench/egress_cost.cpp ]] || { echo "run from the repo root" >&2; exit 1; }
    c++ -std=c++20 -O2 -DNDEBUG -Isrc bench/egress_cost.cpp src/transport/quic_egress.cpp -luring -o "$BIN"
    ns=(unshare --user --map-root-user --net)
    (( EUID == 0 )) && ns=(unshare --net)
    EGRESS_COST_IN_NS=1 exec "${ns[@]}" bash "$0"
fi

ip link set lo up
ip link add dummy0 type dummy
ip addr add 10.99.0.1/24 dev dummy0
ip link set dummy0 up
if [[ -n "$DELAY" ]]; then
    tc qdisc add dev dummy0 root netem delay "$DELAY" limit 400000 \
        || { echo "netem unavailable (sch_netem module)" >&2; exit 1; }
fi

runs() { # runs <mode> <segs> -> one egress_cost line per repetition
    local mode=$1 segs=$2 burst=$2 total r
    (( segs == 1 )) && burst=2
    total=$(( 1500000 * (segs < 12 ? segs : 12) )) # about half a second per run
    [[ -n "$DELAY" ]] && total=$(( 250000 * (segs > 1 ? segs : 2) ))
    for (( r = 0; r < REPS; r++ )); do
        taskset -c "$CPU" "$BIN" "$mode" 10.99.0.2 "$PKT" "$burst" "$segs" "$total" ${SLOTS:+"$SLOTS"}
    done
}

per_send() { # per_send cpu|wall < lines -> median nanoseconds per send
    awk -v what="$1" '{ for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
                        t = (what == "cpu") ? v["user"] + v["sys"] + v["softirq"] : v["wall"]
                        printf "%.0f\n", t * 1e9 / v["sends"] }' \
        | sort -n | awk '{ a[NR] = $1 } END { print a[int((NR + 1) / 2)] }'
}

total_of() { sed -n "s/.* $1=\([0-9]*\).*/\1/p" | awk '{ s += $1 } END { print s + 0 }'; }

echo "# egress_cost $(date)  kernel=$(uname -r)  pkt=$PKT  delay=${DELAY:-none}  zc_slots=${SLOTS:-default}  reps=$REPS  cpu=$CPU"
printf '%-12s | %-10s | %-11s | %-11s | %-10s | %-12s | %-12s | %s\n' \
    "packets/send" "bytes/send" "sendmsg cpu" "SEND_ZC cpu" "difference" "sendmsg wall" "SEND_ZC wall" "copied  waits  errors"
for segs in $SEGS_LIST; do
    sm=$(runs sendmsg "$segs"); zc=$(runs zc "$segs")
    sm_cpu=$(per_send cpu <<<"$sm"); zc_cpu=$(per_send cpu <<<"$zc")
    printf '%-12s | %-10s | %-11s | %-11s | %-+10d | %-12s | %-12s | %s  %s  %s\n' \
        "$segs" "$(( segs * PKT ))" "$sm_cpu" "$zc_cpu" "$(( zc_cpu - sm_cpu ))" \
        "$(per_send wall <<<"$sm")" "$(per_send wall <<<"$zc")" \
        "$(total_of copied <<<"$zc")" "$(total_of waits <<<"$zc")" \
        "$(( $(total_of errors <<<"$sm") + $(total_of errors <<<"$zc") ))"
done
