#!/usr/bin/env bash
# quic_wan.sh — WAN transfer benchmark (embr QUIC SEND_ZC vs sendmsg vs embr TCP vs nc vs scp)
# Run from repo root on BOTH hosts:
#
#   sender:   ROLE=sender   [FILE_SIZE_GB=1] bash bench/quic_wan.sh
#   receiver: ROLE=receiver SENDER_IP=<ip> [SSH_KEY=~/.ssh/key.pem] bash bench/quic_wan.sh
#
# Same harness shape as tcp_wan.sh with three extra rows. Read it as:
#   embrquic vs embrquicsm — same QUIC stack, only the egress differs: io_uring SEND_ZC
#                         (registered slab, NIC reads user pages) vs per-datagram sendmsg.
#                         Loopback cannot show this (it always copies); a real NIC can.
#   embrquic vs embrtcp — same binary, same core (push.cpp/pull.cpp 0 diff),
#                         only the transport and its buffer owner differ
#   scp                 — the encrypted reference; the fair wall-clock peer for QUIC
#   nc                  — plaintext floor; the fair peer for embr TCP
# Both hosts need wolfSSL + ngtcp2 + liburing (bench/setup_quic_deps.sh), a kernel >= 6.2
# and RLIMIT_MEMLOCK >= 1 MiB (assert_sendzc checks both ends). The sender needs inbound
# UDP QUIC_PORT and QUIC_SM_PORT open in its security group, alongside the TCP ports.

set -euo pipefail

ROLE="${ROLE:-}"
BUILD="${BUILD:-./build}"
FILE="${FILE:-/tmp/bench_wan.bin}"
FILE_SIZE_GB="${FILE_SIZE_GB:-1}"
SENDER_IP="${SENDER_IP:-}"
SENDER_USER="${SENDER_USER:-ec2-user}"
SSH_KEY="${SSH_KEY:-}"
TCP_PORT="${TCP_PORT:-10007}"
QUIC_PORT="${QUIC_PORT:-10008}"
QUIC_SM_PORT="${QUIC_SM_PORT:-10009}" # same server, EMBR_QUIC_EGRESS=sendmsg
NC_PORT="${NC_PORT:-9999}"
CERT="${CERT:-/tmp/bench_quic_cert.pem}"
KEY="${KEY:-/tmp/bench_quic_key.pem}"
RUNS="${RUNS:-10}"
WARMUP="${WARMUP:-2}"
CACHE_DISCIPLINE="${CACHE_DISCIPLINE:-drop}"
VERIFY_HASH="${VERIFY_HASH:-1}"
OUT="${OUT:-/tmp/bench_quic_wan_out.bin}"
RESULTS="${RESULTS:-/tmp/bench_quic_wan_results.txt}"
SENDER_STATS="${SENDER_STATS:-/tmp/bench_quic_wan_sender}" # per-push "wall user sys exit" lines, one file per row
RETRY_MAX="${RETRY_MAX:-15}"
RETRY_DELAY="${RETRY_DELAY:-1}"
ROUND_GAP="${ROUND_GAP:-2}"
WOLFSSL_OPTS="${WOLFSSL_OPTS:-$HOME/.local/wolfssl/include/wolfssl/options.h}"
EMBR="${BUILD}/embr"

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=10)
[[ -n "$SSH_KEY" ]] && SSH_OPTS+=(-i "$SSH_KEY")

EXPECTED_BYTES=0
EXPECTED_HASH=""

log()  { echo "[bench_quic_wan $(date +%H:%M:%S)] $*" >&2; }
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

# without --enable-aesgcm-stream wolfSSL memcpys every packet through
# ctx->authBuffer; the copy-floor row this bench exists to measure would be false
assert_toolchain() {
    [[ -f "$WOLFSSL_OPTS" ]] || die "wolfSSL options.h not found at $WOLFSSL_OPTS — run bench/setup_quic_deps.sh"
    grep -q WOLFSSL_AESGCM_STREAM "$WOLFSSL_OPTS" \
        || die "wolfSSL built without --enable-aesgcm-stream — QUIC numbers would be meaningless"
    [[ "$BUILD" == *debug* || "$BUILD" == *asan* ]] \
        && die "BUILD=$BUILD is not a release build — never quote a bench number from it"
    return 0
}

# embr asks for 4 MiB UDP buffers; the kernel clamps to rmem_max/wmem_max (212992 on
# AL2023). Below that the receiver's per-chunk SHA-256 stall overflows the buffer at WAN
# rates and every chunk becomes a loss event — the number would measure a sysctl, not QUIC.
SOCKBUF_MIN=4194304
assert_sockbuf() { # assert_sockbuf local|remote
    local where="$1" r w
    if [[ "$where" == "remote" ]]; then
        r=$(ssh "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}" sysctl -n net.core.rmem_max 2>/dev/null || echo 0)
        w=$(ssh "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}" sysctl -n net.core.wmem_max 2>/dev/null || echo 0)
    else
        r=$(sysctl -n net.core.rmem_max); w=$(sysctl -n net.core.wmem_max)
    fi
    (( r >= SOCKBUF_MIN && w >= SOCKBUF_MIN )) \
        || die "$where net.core.rmem_max/wmem_max = $r/$w < $SOCKBUF_MIN — run bench/setup_quic_deps.sh there (or: sudo sysctl -w net.core.rmem_max=16777216 net.core.wmem_max=16777216)"
    log "$where sockbuf ok: rmem_max=$r wmem_max=$w"
}

# the SEND_ZC slab registers ~105 KiB of RLIMIT_MEMLOCK per connection; below 1 MiB embr
# silently runs sendmsg and the embrquic row measures nothing new. IORING_SEND_ZC_REPORT_USAGE
# is kernel >= 6.2: older kernels fail every SEND_ZC with EINVAL and the row dies outright
assert_sendzc() { # assert_sendzc local|remote
    local where="$1" k l
    if [[ "$where" == "remote" ]]; then
        k=$(ssh "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}" uname -r 2>/dev/null || echo 0)
        l=$(ssh "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}" 'ulimit -l' 2>/dev/null || echo 0)
    else
        k=$(uname -r); l=$(ulimit -l)
    fi
    local maj="${k%%.*}" min; min="${k#*.}"; min="${min%%.*}"
    (( maj > 6 || (maj == 6 && min >= 2) )) \
        || die "$where kernel $k < 6.2 — SEND_ZC REPORT_USAGE unsupported; embrquic would fail with EINVAL"
    [[ "$l" == "unlimited" ]] || (( l >= 1024 )) \
        || die "$where ulimit -l = $l KiB < 1 MiB — the SEND_ZC slab cannot register; embrquic would be a sendmsg run"
    log "$where sendzc ok: kernel=$k memlock=${l}KiB"
}

# fail on the real cause first: every remote preflight below would otherwise report a bogus 0
assert_ssh() {
    ssh "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}" true 2>/dev/null \
        || die "cannot ssh ${SENDER_USER}@${SENDER_IP} — check SSH_KEY, key mode 600, and TCP 22 in the sender's security group"
    log "ssh ok: ${SENDER_USER}@${SENDER_IP}"
}

# a closed TCP port would only show as RETRY_MAX slow failures mid-bench; probe before the clock starts
# UDP cannot be probed this way: the first embrquic warmup is the probe for $QUIC_PORT/$QUIC_SM_PORT
assert_ports() {
    local port
    for port in "$TCP_PORT" "$NC_PORT"; do
        timeout 5 bash -c "</dev/tcp/$SENDER_IP/$port" 2>/dev/null \
            || die "TCP $port on $SENDER_IP not reachable — sender not started, or port missing from its security group"
    done
    log "tcp ports ok: $TCP_PORT $NC_PORT (udp $QUIC_PORT $QUIC_SM_PORT are probed by the first warmup)"
}

# the client runs SSL_VERIFY_NONE, so a throwaway self-signed cert is all TLS 1.3 needs
ensure_cert() {
    [[ -f "$CERT" && -f "$KEY" ]] && return 0
    command -v openssl >/dev/null || die "openssl binary missing: sudo dnf install -y openssl"
    log "generating self-signed cert -> $CERT"
    openssl req -x509 -newkey rsa:2048 -nodes -days 7 -subj /CN=embr-bench \
        -keyout "$KEY" -out "$CERT" 2>/dev/null || die "openssl req failed"
    chmod 600 "$KEY"
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
    [[ -x "$EMBR" ]] || die "embr binary not found at $EMBR — run cmake --build $BUILD first"
    assert_toolchain
    assert_sockbuf local
    assert_sendzc local
    ensure_cert
    if [[ ! -f "$FILE" ]]; then
        log "generating ${FILE_SIZE_GB}GB random file -> $FILE"
        dd if=/dev/urandom of="$FILE" bs=1M count=$(( FILE_SIZE_GB*1024 )) status=progress
    fi
    log "file $(du -h "$FILE" | cut -f1)  sha256=$(sha256sum "$FILE" | cut -d' ' -f1)"

    # the sender's sys time per GiB is the copy-floor number: SEND_ZC vs sendmsg differ only there
    rm -f "$SENDER_STATS".embrquic "$SENDER_STATS".embrquicsm "$SENDER_STATS".embrtcp "$SENDER_STATS".embrquic.log
    local T=(/usr/bin/time -f "%e %U %S %x" -a -o) # wall user sys exit; the summary keeps exit==0 only
    # EMBR_QUIC_STATS=1 makes embr print one [quic-egress] line per connection; copied==0 proves real zero-copy
    ( while true; do EMBR_QUIC_STATS=1 "${T[@]}" "$SENDER_STATS.embrquic" "$EMBR" push "$FILE" --port "$QUIC_PORT" --transport quic \
          --cert "$CERT" --key "$KEY" >/dev/null 2>>"$SENDER_STATS.embrquic.log" || sleep 0.3; done ) &
    SENDER_PIDS+=($!)
    ( while true; do EMBR_QUIC_EGRESS=sendmsg "${T[@]}" "$SENDER_STATS.embrquicsm" "$EMBR" push "$FILE" --port "$QUIC_SM_PORT" --transport quic \
          --cert "$CERT" --key "$KEY" >/dev/null 2>&1 || sleep 0.3; done ) &
    SENDER_PIDS+=($!)
    ( while true; do "${T[@]}" "$SENDER_STATS.embrtcp" "$EMBR" push "$FILE" --port "$TCP_PORT" --transport tcp \
          >/dev/null 2>&1 || sleep 0.3; done ) &
    SENDER_PIDS+=($!)
    ( while true; do ncat -l "$NC_PORT" --send-only < "$FILE" >/dev/null 2>&1 || sleep 0.3; done ) &
    SENDER_PIDS+=($!)
    trap stop_sender EXIT INT TERM
    log "servers up — quic:udp/$QUIC_PORT  quicsm:udp/$QUIC_SM_PORT  tcp:$TCP_PORT  nc:$NC_PORT  scp via sshd  (pids ${SENDER_PIDS[*]})"
    log "Ctrl-C when receiver is done — the sender summary prints on exit"
    wait
}

# a loop subshell dies with the trap but the push (and its time wrapper) under it would not
kill_tree() {
    local p c
    for p in "$@"; do
        for c in $(pgrep -P "$p" 2>/dev/null); do kill_tree "$c"; done
        kill "$p" 2>/dev/null || true
    done
}

SENDER_PIDS=()
stop_sender() { # runs once: Ctrl-C and the EXIT that follows it must not print the summary twice
    trap - EXIT INT TERM
    kill_tree "${SENDER_PIDS[@]}"
    sender_summary
}

# medians of the per-push lines; the first WARMUP transfers per row are the receiver's warmups
# (same WARMUP default on both ends; pass the receiver's value here if it was changed)
sender_summary() {
    log "=== sender summary (median over completed pushes, warmups excluded) ==="
    printf '\n%-10s | %-6s | %-8s | %-8s\n' row n "user s" "sys s" # wall includes the accept wait, so it is not shown
    for row in embrquic embrquicsm embrtcp; do
        local f="$SENDER_STATS.$row"
        [[ -s "$f" ]] || { printf '%-10s | (no data)\n' "$row"; continue; }
        # a push killed by the exit trap leaves "Command terminated by signal" and a zero line: drop both
        local lines; lines=$(awk '/terminated by signal/ { skip=1; next } skip { skip=0; next } NF==4 && $4==0' "$f" | tail -n +$(( WARMUP + 1 )))
        [[ -n "$lines" ]] || { printf '%-10s | (warmup only)\n' "$row"; continue; }
        printf '%-10s | %-6s | %-8s | %-8s\n' "$row" "$(wc -l <<<"$lines")" \
            "$(awk '{print $2}' <<<"$lines" | median)" "$(awk '{print $3}' <<<"$lines" | median)"
    done
    local st="$SENDER_STATS.embrquic.log"
    if grep -q '^\[quic-egress\]' "$st" 2>/dev/null; then
        # sum over connections: copied/sends is the share the kernel copied anyway; want ~0 on a real NIC
        awk '/^\[quic-egress\]/ { for (i=2;i<=NF;i++) { split($i,kv,"="); t[kv[1]]+=kv[2] } n++ }
             END { printf "embrquic egress over %d connections: sends=%d copied=%d (%.1f%%) fallback=%d errors=%d\n",
                   n, t["sends"], t["copied"], t["sends"]?100*t["copied"]/t["sends"]:0, t["fallback"], t["errors"] }' "$st" >&2
    else
        log "no [quic-egress] lines in $st — binary predates EMBR_QUIC_STATS; copied ratio unknown"
    fi
    log "raw per-push lines in $SENDER_STATS.<row>; read: embrquic sys vs embrquicsm sys = SEND_ZC saving per GiB"
}

run_receiver() {
    [[ -n "$SENDER_IP" ]] || die "set SENDER_IP"
    [[ -x "$EMBR" ]] || die "embr binary not found at $EMBR"
    command -v /usr/bin/time >/dev/null || die "GNU time missing: sudo dnf install -y time"
    assert_toolchain
    assert_ssh
    assert_ports
    assert_sockbuf local
    assert_sockbuf remote
    assert_sendzc local
    assert_sendzc remote

    log "RECEIVER — ${SENDER_USER}@${SENDER_IP} — fetching source size+hash..."
    local meta
    meta=$(ssh "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}" \
        "stat -c%s '$FILE'; sha256sum '$FILE' | cut -d' ' -f1") \
        || die "cannot reach sender — check SSH key and that $FILE exists on sender"
    EXPECTED_BYTES=$(sed -n 1p <<< "$meta")
    EXPECTED_HASH=$(sed -n 2p <<< "$meta")
    [[ "$EXPECTED_BYTES" -gt 0 ]] 2>/dev/null || die "source file empty/missing on sender"
    log "expected $EXPECTED_BYTES bytes  sha256=$EXPECTED_HASH"

    { echo "# bench_quic_wan $(date)"
      echo "# sender=$SENDER_IP cache=$CACHE_DISCIPLINE bytes=$EXPECTED_BYTES build=$BUILD"
      echo "# kernel local=$(uname -r) remote=$(ssh "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}" uname -r 2>/dev/null || echo NA)"
      echo "# order: embrquic->embrquicsm->embrtcp->nc->scp"
      echo "# embrquic:   ngtcp2+wolfSSL, mmap Buffer + in-place AEAD, io_uring SEND_ZC egress (registered slab)"
      echo "# embrquicsm: same stack, EMBR_QUIC_EGRESS=sendmsg on the sender (per-datagram copy baseline)"
      echo "# embrtcp:    sendfile/splice, plaintext.  nc: plaintext floor.  scp: encrypted reference"
      echo ""; } > "$RESULTS"

    local total=$(( RUNS + WARMUP ))
    for (( i=1; i<=total; i++ )); do
        local label="run$i"; (( i <= WARMUP )) && label="warmup$i"
        log "=== $label ($i/$total) ==="
        run_tool "${label}_embrquic"   "$EMBR" pull "$SENDER_IP" --port "$QUIC_PORT"    --transport quic --out "$OUT"
        run_tool "${label}_embrquicsm" "$EMBR" pull "$SENDER_IP" --port "$QUIC_SM_PORT" --transport quic --out "$OUT"
        run_tool "${label}_embrtcp"  "$EMBR" pull "$SENDER_IP" --port "$TCP_PORT"  --transport tcp  --out "$OUT"
        run_tool "${label}_nc"       bash -c "ncat '$SENDER_IP' '$NC_PORT' --recv-only > '$OUT'"
        run_tool "${label}_scp"      scp "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}:${FILE}" "$OUT"
        echo "" >> "$RESULTS"
        sleep "$ROUND_GAP"
    done
    summarize
}

summarize() {
    set +e
    log "=== raw results ==="; cat "$RESULTS" >&2
    log "=== summary (warmup excluded) — median [min..max] ==="
    printf '\n%-10s | %-18s | %-18s | %-8s | %-8s\n' tool "throughput Gbps" "wall sec" "user s" "sys s"
    printf -- '-----------+--------------------+--------------------+----------+---------\n'
    for tool in embrquic embrquicsm embrtcp nc scp; do
        local lines gbpss secs users syss
        lines=$(grep -E "^run[0-9]+_${tool} " "$RESULTS")
        [[ -z "$lines" ]] && { printf '%-10s | (no data)\n' "$tool"; continue; }
        gbpss=$(sed -n 's/.* gbps=\([^ ]*\).*/\1/p'      <<< "$lines")
        secs=$( sed -n 's/.* sec=\([^ ]*\).*/\1/p'       <<< "$lines")
        users=$(sed -n 's/.* user=\([0-9.]*\)s.*/\1/p'   <<< "$lines")
        syss=$( sed -n 's/.* sys=\([0-9.]*\)s.*/\1/p'    <<< "$lines")
        printf '%-10s | %-7s [%s] | %-7s [%s] | %-8s | %-8s\n' "$tool" \
            "$(median <<<"$gbpss")" "$(minmax <<<"$gbpss")" \
            "$(median <<<"$secs")"  "$(minmax <<<"$secs")"  \
            "$(median <<<"$users")" "$(median <<<"$syss")"
    done
    set -e
    log "raw results in $RESULTS"
    log "read: embrquic vs embrquicsm = SEND_ZC vs sendmsg, receiver-side wall; the sender's sys s"
    log "      is the copy-floor number (run \`time\` around the push loop there if you need it);"
    log "      embrquic vs embrtcp Gbps = transport cost at the path ceiling;"
    log "      embrquic vs scp = encrypted-vs-encrypted; sys s = kernel work per GiB"
}

case "$ROLE" in
    sender)   run_sender ;;
    receiver) run_receiver ;;
    *)
        echo "Usage (from repo root):"
        echo "  ROLE=sender   [FILE_SIZE_GB=1] [BUILD=./build] bash bench/quic_wan.sh"
        echo "  ROLE=receiver SENDER_IP=<ip> [SSH_KEY=~/.ssh/key.pem] bash bench/quic_wan.sh"
        echo "Sender security group: inbound TCP $TCP_PORT, $NC_PORT, 22 and UDP $QUIC_PORT, $QUIC_SM_PORT"
        exit 1 ;;
esac
