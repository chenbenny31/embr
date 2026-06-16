#!/usr/bin/env bash
# bench_loopback.sh — loopback transfer benchmark (embr vs nc vs scp)
# Run from repo root: bash bench/bench_loopback.sh
#
# Measures SOFTWARE overhead — sender + receiver CPU — not network throughput.
# Zero-copy signal: embr send_sys near-zero; nc send_user high (read/write loop).

set -euo pipefail

BUILD="${BUILD:-./build}"
FILE="${FILE:-/tmp/bench_loopback.bin}"
FILE_SIZE_GB="${FILE_SIZE_GB:-1}"
EMBR_PORT="${EMBR_PORT:-10007}"
NC_PORT="${NC_PORT:-9999}"
RUNS="${RUNS:-5}"
WARMUP="${WARMUP:-1}"
CACHE_DISCIPLINE="${CACHE_DISCIPLINE:-drop}"
VERIFY_HASH="${VERIFY_HASH:-1}"
OUT="${OUT:-/tmp/bench_loopback_out.bin}"
RESULTS="${RESULTS:-/tmp/bench_loopback_results.txt}"
EMBR="${BUILD}/embr"

EXPECTED_BYTES=0
EXPECTED_HASH=""

log() { echo "[bench_loopback $(date +%H:%M:%S)] $*" >&2; }
die() { log "FATAL: $*"; exit 1; }

wall_to_sec() {
    awk -F: '{ if(NF==3) print $1*3600+$2*60+$3;
               else if(NF==2) print $1*60+$2; else print $1 }' <<< "$1"
}
median() { sort -n | awk '{a[NR]=$1}
    END{ if(NR==0)print "NA"; else if(NR%2)print a[(NR+1)/2];
         else printf "%.4f\n",(a[NR/2]+a[NR/2+1])/2 }'; }
minmax() { sort -n | awk 'NR==1{mn=$1}{mx=$1}
    END{ if(NR==0)print "NA NA"; else printf "%s %s\n",mn,mx }'; }

prepare_cache() {
    [[ "$CACHE_DISCIPLINE" == "drop" ]] || return 0
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1 || true
}

verify_output() {
    [[ -f "$OUT" ]] || { log "  verify: output missing"; return 1; }
    local got; got=$(stat -c%s "$OUT")
    [[ "$got" -eq "$EXPECTED_BYTES" ]] || { log "  verify: size $got != $EXPECTED_BYTES"; return 1; }
    if [[ "$VERIFY_HASH" == "1" ]]; then
        [[ "$(sha256sum "$OUT" | cut -d' ' -f1)" == "$EXPECTED_HASH" ]] \
            || { log "  verify: hash mismatch"; return 1; }
    fi
    return 0
}

# run_transfer <label> <sender_cmd_string> <receiver_cmd_string>
# sender runs in background (timed), receiver runs in foreground (timed)
# both commands executed via bash -c so redirection works naturally
run_transfer() {
    local label="$1"
    local send_cmd="$2"
    local recv_cmd="$3"

    rm -f "$OUT"
    prepare_cache

    local stfile rfile rc=0
    stfile=$(mktemp); rfile=$(mktemp)

    # sender in background, timed
    /usr/bin/time -v -o "$stfile" bash -c "$send_cmd" >/dev/null 2>&1 &
    local spid=$!

    # give sender time to reach listen state
    sleep 0.5

    # receiver in foreground, timed
    /usr/bin/time -v -o "$rfile" bash -c "$recv_cmd" >/dev/null 2>&1 || rc=$?
    wait "$spid" 2>/dev/null || true

    if [[ $rc -eq 0 ]] && verify_output; then
        local swall suser ssys rwall ruser rsys sec gbps
        swall=$(grep -F "Elapsed (wall"    "$stfile" | awk '{print $NF}' || echo NA)
        suser=$(grep -F "User time"        "$stfile" | awk '{print $NF}' || echo NA)
        ssys=$( grep -F "System time"      "$stfile" | awk '{print $NF}' || echo NA)
        rwall=$(grep -F "Elapsed (wall"    "$rfile"  | awk '{print $NF}' || echo NA)
        ruser=$(grep -F "User time"        "$rfile"  | awk '{print $NF}' || echo NA)
        rsys=$( grep -F "System time"      "$rfile"  | awk '{print $NF}' || echo NA)
        sec=$(wall_to_sec "$rwall")
        gbps=$(awk -v b="$EXPECTED_BYTES" -v s="$sec" \
            'BEGIN{ if(s>0)printf "%.2f",b*8/s/1e9; else print "NA" }')
        printf '%s wall=%s sec=%s gbps=%s send_user=%ss send_sys=%ss recv_user=%ss recv_sys=%ss\n' \
            "$label" "$rwall" "$sec" "$gbps" \
            "$suser" "$ssys" "$ruser" "$rsys" | tee -a "$RESULTS" >&2
        rm -f "$stfile" "$rfile"; return 0
    fi
    log "  $label failed (rc=$rc)"
    rm -f "$stfile" "$rfile"; return 1
}

main() {
    [[ -x "$EMBR" ]] || die "embr binary not found at $EMBR — run cmake --build build first"
    command -v /usr/bin/time >/dev/null || die "GNU time missing: sudo dnf install -y time"
    command -v ncat >/dev/null          || die "ncat missing: sudo dnf install -y nmap-ncat"

    if [[ ! -f "$FILE" ]]; then
        log "generating ${FILE_SIZE_GB}GB random file -> $FILE"
        dd if=/dev/urandom of="$FILE" bs=1M count=$(( FILE_SIZE_GB*1024 )) status=progress
    fi
    EXPECTED_BYTES=$(stat -c%s "$FILE")
    EXPECTED_HASH=$(sha256sum "$FILE" | cut -d' ' -f1)
    log "file $(du -h "$FILE" | cut -f1)  sha256=$EXPECTED_HASH"

    { echo "# bench_loopback $(date)"
      echo "# file=${FILE_SIZE_GB}GB cache=$CACHE_DISCIPLINE"
      echo "# send_user/sys = sender CPU (zero-copy signal); recv_user/sys = receiver CPU"
      echo "# scp via localhost SSH — reference only, encrypted"
      echo ""; } > "$RESULTS"

    local total=$(( RUNS + WARMUP ))
    for (( i=1; i<=total; i++ )); do
        local label="run$i"; (( i <= WARMUP )) && label="warmup$i"
        log "=== $label ($i/$total) ==="

        # ── embr ──────────────────────────────────────────────────────────────
        log "embr..."
        run_transfer "${label}_embr" \
            "$EMBR push $FILE --port $EMBR_PORT" \
            "$EMBR pull 127.0.0.1 --port $EMBR_PORT --out $OUT" || true

        # ── nc ────────────────────────────────────────────────────────────────
        log "nc..."
        run_transfer "${label}_nc" \
            "ncat -l $NC_PORT --send-only < $FILE" \
            "ncat 127.0.0.1 $NC_PORT --recv-only > $OUT" || true

        # ── scp (localhost SSH — encrypted, reference only) ───────────────────
        log "scp (reference)..."
        run_transfer "${label}_scp" \
            "true" \
            "scp -o StrictHostKeyChecking=no $(whoami)@127.0.0.1:$FILE $OUT" || true

        echo "" >> "$RESULTS"
    done

    summarize
}

summarize() {
    set +e
    log "=== raw results ==="; cat "$RESULTS" >&2
    log "=== summary (warmup excluded) — zero-copy signal is send_sys ==="
    printf '\n%-6s | %-8s | %-10s | %-10s | %-10s | %-10s\n' \
        tool "Gbps" "send_user" "send_sys" "recv_user" "recv_sys"
    printf -- '-------+----------+------------+------------+------------+-----------\n'
    for tool in embr nc scp; do
        local lines gbpss su ss ru rs
        lines=$(grep -E "^run[0-9]+_${tool} " "$RESULTS" || true)
        [[ -z "$lines" ]] && { printf '%-6s | (no data)\n' "$tool"; continue; }
        gbpss=$(sed -n 's/.* gbps=\([^ ]*\).*/\1/p'         <<< "$lines")
        su=$(   sed -n 's/.* send_user=\([0-9.]*\)s.*/\1/p' <<< "$lines")
        ss=$(   sed -n 's/.* send_sys=\([0-9.]*\)s.*/\1/p'  <<< "$lines")
        ru=$(   sed -n 's/.* recv_user=\([0-9.]*\)s.*/\1/p' <<< "$lines")
        rs=$(   sed -n 's/.* recv_sys=\([0-9.]*\)s.*/\1/p'  <<< "$lines")
        printf '%-6s | %-8s | %-10s | %-10s | %-10s | %-10s\n' "$tool" \
            "$(median <<<"$gbpss")" \
            "$(median <<<"$su")"    \
            "$(median <<<"$ss")"    \
            "$(median <<<"$ru")"    \
            "$(median <<<"$rs")"
    done
    set -e
    log "raw results in $RESULTS"
}

main "$@"
