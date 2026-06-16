#!/usr/bin/env bash
# bench_wan.sh — WAN transfer benchmark (embr vs nc vs scp)
# Run from repo root: ROLE=sender bash bench/bench_wan.sh
#
#   sender:   ROLE=sender   [FILE_SIZE_GB=1] bash bench/bench_wan.sh
#   receiver: ROLE=receiver SENDER_IP=<ip>  bash bench/bench_wan.sh

set -euo pipefail

ROLE="${ROLE:-}"
BUILD="${BUILD:-./build}"
FILE="${FILE:-/tmp/bench_wan.bin}"
FILE_SIZE_GB="${FILE_SIZE_GB:-1}"
SENDER_IP="${SENDER_IP:-}"
SENDER_USER="${SENDER_USER:-ec2-user}"
SSH_KEY="${SSH_KEY:-}"
EMBR_PORT="${EMBR_PORT:-10007}"
NC_PORT="${NC_PORT:-9999}"
RUNS="${RUNS:-10}"
WARMUP="${WARMUP:-2}"
CACHE_DISCIPLINE="${CACHE_DISCIPLINE:-drop}"
VERIFY_HASH="${VERIFY_HASH:-1}"
OUT="${OUT:-/tmp/bench_wan_out.bin}"
RESULTS="${RESULTS:-/tmp/bench_wan_results.txt}"
RETRY_MAX="${RETRY_MAX:-15}"
RETRY_DELAY="${RETRY_DELAY:-1}"
ROUND_GAP="${ROUND_GAP:-2}"
EMBR="${BUILD}/embr"

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=10)
[[ -n "$SSH_KEY" ]] && SSH_OPTS+=(-i "$SSH_KEY")

EXPECTED_BYTES=0
EXPECTED_HASH=""

log()  { echo "[bench_wan $(date +%H:%M:%S)] $*" >&2; }
die()  { log "FATAL: $*"; exit 1; }

wall_to_sec() {
    awk -F: '{ if(NF==3) print $1*3600+$2*60+$3;
               else if(NF==2) print $1*60+$2; else print $1 }' <<< "$1"
}
median() { sort -n | awk '{a[NR]=$1}
    END{ if(NR==0)print "NA"; else if(NR%2)print a[(NR+1)/2];
         else printf "%.4f\n",(a[NR/2]+a[NR/2+1])/2 }'; }
minmax() { sort -n | awk 'NR==1{mn=$1}{mx=$1}
    END{ if(NR==0)print "NA NA"; else printf "%s %s\n",mn,mx }'; }

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

prepare_cache() {
    [[ "$CACHE_DISCIPLINE" == "drop" ]] || return 0
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1 || true
    ssh "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}" \
        "sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null" 2>/dev/null || true
}

run_tool() {
    local label="$1"; shift
    local attempt=0
    while (( attempt < RETRY_MAX )); do
        attempt=$(( attempt + 1 ))
        rm -f "$OUT"; prepare_cache
        local terr ttime rc=0
        terr=$(mktemp); ttime=$(mktemp)
        /usr/bin/time -v -o "$ttime" "$@" >/dev/null 2>"$terr" || rc=$?
        if [[ $rc -eq 0 ]] && verify_output; then
            local wall user sys rss sec gbps
            wall=$(grep -F "Elapsed (wall"    "$ttime" | awk '{print $NF}' || echo NA)
            user=$(grep -F "User time"        "$ttime" | awk '{print $NF}' || echo NA)
            sys=$( grep -F "System time"      "$ttime" | awk '{print $NF}' || echo NA)
            rss=$( grep -F "Maximum resident" "$ttime" | awk '{print $NF}' || echo NA)
            sec=$(wall_to_sec "$wall")
            gbps=$(awk -v b="$EXPECTED_BYTES" -v s="$sec" \
                'BEGIN{ if(s>0)printf "%.2f",b*8/s/1e9; else print "NA" }')
            printf '%s wall=%s sec=%s gbps=%s user=%ss sys=%ss rss=%sKB\n' \
                "$label" "$wall" "$sec" "$gbps" "$user" "$sys" "$rss" | tee -a "$RESULTS" >&2
            rm -f "$terr" "$ttime"; return 0
        fi
        log "  $label attempt $attempt failed (rc=$rc):"
        sed 's/^/      /' "$terr" >&2
        rm -f "$terr" "$ttime"
        sleep "$RETRY_DELAY"
    done
    die "$label failed after $RETRY_MAX attempts"
}

run_sender() {
    log "SENDER mode — binary: $EMBR"
    [[ -x "$EMBR" ]] || die "embr binary not found at $EMBR — run cmake --build build first"
    if [[ ! -f "$FILE" ]]; then
        log "generating ${FILE_SIZE_GB}GB random file -> $FILE"
        dd if=/dev/urandom of="$FILE" bs=1M count=$(( FILE_SIZE_GB*1024 )) status=progress
    fi
    log "file $(du -h "$FILE" | cut -f1)  sha256=$(sha256sum "$FILE" | cut -d' ' -f1)"

    ( while true; do "$EMBR" push "$FILE" --port "$EMBR_PORT" >/dev/null 2>&1 || sleep 0.3; done ) &
    local ep=$!
    ( while true; do ncat -l "$NC_PORT" --send-only < "$FILE" >/dev/null 2>&1 || sleep 0.3; done ) &
    local np=$!
    trap 'kill $ep $np 2>/dev/null || true' EXIT INT TERM
    log "servers up — embr($ep):$EMBR_PORT  nc($np):$NC_PORT  scp via sshd"
    log "Ctrl-C when receiver is done"
    wait
}

run_receiver() {
    [[ -n "$SENDER_IP" ]] || die "set SENDER_IP"
    [[ -x "$EMBR" ]] || die "embr binary not found at $EMBR"
    command -v /usr/bin/time >/dev/null || die "GNU time missing: sudo dnf install -y time"

    local ssh_key_opt=""
    [[ -n "$SSH_KEY" ]] && ssh_key_opt="-i $SSH_KEY"

    log "RECEIVER — ${SENDER_USER}@${SENDER_IP} — fetching source size+hash..."
    local meta
    meta=$(ssh "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}" \
        "stat -c%s '$FILE'; sha256sum '$FILE' | cut -d' ' -f1") \
        || die "cannot reach sender — check SSH key and that $FILE exists on sender"
    EXPECTED_BYTES=$(sed -n 1p <<< "$meta")
    EXPECTED_HASH=$(sed -n 2p <<< "$meta")
    [[ "$EXPECTED_BYTES" -gt 0 ]] 2>/dev/null || die "source file empty/missing on sender"
    log "expected $EXPECTED_BYTES bytes  sha256=$EXPECTED_HASH"

    { echo "# bench_wan $(date)"
      echo "# sender=$SENDER_IP cache=$CACHE_DISCIPLINE bytes=$EXPECTED_BYTES"
      echo "# order: embr->nc->scp (scp encrypted = reference only)"
      echo ""; } > "$RESULTS"

    local total=$(( RUNS + WARMUP ))
    for (( i=1; i<=total; i++ )); do
        local label="run$i"; (( i <= WARMUP )) && label="warmup$i"
        log "=== $label ($i/$total) ==="
        run_tool "${label}_embr" "$EMBR" pull "$SENDER_IP" --port "$EMBR_PORT" --out "$OUT"
        run_tool "${label}_nc"   bash -c "ncat '$SENDER_IP' '$NC_PORT' --recv-only > '$OUT'"
        run_tool "${label}_scp"  scp "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}:${FILE}" "$OUT"
        echo "" >> "$RESULTS"
        sleep "$ROUND_GAP"
    done
    summarize
}

summarize() {
    set +e
    log "=== raw results ==="; cat "$RESULTS" >&2
    log "=== summary (warmup excluded) — median [min..max] ==="
    printf '\n%-6s | %-18s | %-18s | %-8s\n' tool "throughput Gbps" "wall sec" "cpu s"
    printf -- '-------+--------------------+--------------------+---------\n'
    for tool in embr nc scp; do
        local lines gbpss secs cpus
        lines=$(grep -E "^run[0-9]+_${tool} " "$RESULTS")
        gbpss=$(sed -n 's/.* gbps=\([^ ]*\).*/\1/p' <<< "$lines")
        secs=$( sed -n 's/.* sec=\([^ ]*\).*/\1/p'  <<< "$lines")
        cpus=$( sed -n 's/.* user=\([0-9.]*\)s sys=\([0-9.]*\)s.*/\1 \2/p' \
                <<< "$lines" | awk '{print $1+$2}')
        printf '%-6s | %-7s [%s] | %-7s [%s] | %s\n' "$tool" \
            "$(median <<<"$gbpss")" "$(minmax <<<"$gbpss")" \
            "$(median <<<"$secs")"  "$(minmax <<<"$secs")"  \
            "$(median <<<"$cpus")"
    done
    set -e
    log "raw results in $RESULTS"
}

case "$ROLE" in
    sender)   run_sender ;;
    receiver) run_receiver ;;
    *)
        echo "Usage (from repo root):"
        echo "  ROLE=sender   [FILE_SIZE_GB=1] [BUILD=./build] bash bench/bench_wan.sh"
        echo "  ROLE=receiver SENDER_IP=<ip> [SSH_KEY=~/.ssh/key.pem] bash bench/bench_wan.sh"
        exit 1 ;;
esac
