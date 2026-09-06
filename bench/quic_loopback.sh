#!/usr/bin/env bash
# quic_loopback.sh — loopback transfer benchmark (embr QUIC vs embr TCP vs nc)
# Run from repo root: bash bench/quic_loopback.sh
#
# Measures SOFTWARE overhead — sender + receiver CPU — not network throughput.
# The axis here is transport, not tool: identical embr binary, identical core
# (push.cpp/pull.cpp are 0 diff between the two), only the Buffer owner differs.
#
# QUIC is encrypted and TCP is plaintext, so wall/Gbps is NOT apples-to-apples.
# Read sys CPU and the copy-count story; treat throughput as context only.
#
# embrquic vs embrquicsm is the SEND_ZC egress against its sendmsg baseline. On loopback
# the kernel cannot pin user pages for the receiver, so every SEND_ZC completes as
# ZC_COPIED: the row pays the copy AND the notification CQEs, and lands ~10% behind
# embrquicsm (2026-09-05, release, 1 GiB, 5 runs: 3.21-3.30 vs 3.55-3.67 Gbps, sender sys
# 1.05-1.11 s vs 0.96-0.99 s). That is the expected loopback result, not a regression.
# This bench proves the path is wired and shows the copy floor; a ZC gain is a WAN claim.

set -euo pipefail

BUILD="${BUILD:-./cmake-build-release}"
FILE="${FILE:-/tmp/bench_loopback.bin}"
FILE_SIZE_GB="${FILE_SIZE_GB:-1}"
TCP_PORT="${TCP_PORT:-10007}"
QUIC_PORT="${QUIC_PORT:-10008}"
NC_PORT="${NC_PORT:-9999}"
CERT="${CERT:-cert.pem}"
KEY="${KEY:-key.pem}"
RUNS="${RUNS:-5}"
WARMUP="${WARMUP:-1}"
CACHE_DISCIPLINE="${CACHE_DISCIPLINE:-drop}"
VERIFY_HASH="${VERIFY_HASH:-1}"
OUT="${OUT:-/tmp/bench_quic_out.bin}"
RESULTS="${RESULTS:-/tmp/bench_quic_results.txt}"
EMBR="${BUILD}/embr"
WOLFSSL_OPTS="${WOLFSSL_OPTS:-$HOME/.local/wolfssl/include/wolfssl/options.h}"

EXPECTED_BYTES=0
EXPECTED_HASH=""

log() { echo "[bench_quic $(date +%H:%M:%S)] $*" >&2; }
die() { log "FATAL: $*"; exit 1; }

wall_to_sec() {
    awk -F: '{ if(NF==3) print $1*3600+$2*60+$3;
               else if(NF==2) print $1*60+$2; else print $1 }' <<< "$1"
}
median() { sort -n | awk '{a[NR]=$1}
    END{ if(NR==0)print "NA"; else if(NR%2)print a[(NR+1)/2];
         else printf "%.4f\n",(a[NR/2]+a[NR/2+1])/2 }'; }

prepare_cache() {
    [[ "$CACHE_DISCIPLINE" == "drop" ]] || return 0
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1 || true
}

# without --enable-aesgcm-stream wolfSSL memcpys every packet through
# ctx->authBuffer, which silently invalidates the whole comparison
assert_toolchain() {
    [[ -f "$WOLFSSL_OPTS" ]] || die "wolfSSL options.h not found at $WOLFSSL_OPTS"
    grep -q WOLFSSL_AESGCM_STREAM "$WOLFSSL_OPTS" \
        || die "wolfSSL built without --enable-aesgcm-stream — QUIC numbers would be meaningless"
    [[ "$BUILD" == *debug* || "$BUILD" == *asan* ]] \
        && die "BUILD=$BUILD is not a release build — never quote a bench number from it"
    return 0
}

# embr asks for 4 MiB UDP buffers; the kernel clamps to rmem_max/wmem_max
assert_sockbuf() {
    local r w; r=$(sysctl -n net.core.rmem_max); w=$(sysctl -n net.core.wmem_max)
    (( r >= 4194304 && w >= 4194304 )) \
        || die "net.core.rmem_max/wmem_max = $r/$w < 4194304 — sudo sysctl -w net.core.rmem_max=16777216 net.core.wmem_max=16777216"
}

# the SEND_ZC slab registers ~105 KiB of RLIMIT_MEMLOCK per connection (64 slots); below
# 1 MiB embr silently runs sendmsg and the embrquic row measures nothing new.
# IORING_SEND_ZC_REPORT_USAGE is kernel >= 6.2; older kernels reject every SEND_ZC with
# EINVAL and embr copies each datagram through send(): a row, but not the one you wanted
assert_sendzc() {
    local k l; k=$(uname -r); l=$(ulimit -l)
    local maj="${k%%.*}" min; min="${k#*.}"; min="${min%%.*}"
    (( maj > 6 || (maj == 6 && min >= 2) )) \
        || die "kernel $k < 6.2 — SEND_ZC REPORT_USAGE unsupported; embrquic would be a copy-fallback run"
    [[ "$l" == "unlimited" ]] || (( l >= 1024 )) \
        || die "ulimit -l = $l KiB < 1 MiB — the SEND_ZC slab cannot register; embrquic would be a sendmsg run"
    log "sendzc ok: kernel=$k memlock=${l}KiB"
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
run_transfer() {
    local label="$1" send_cmd="$2" recv_cmd="$3"

    rm -f "$OUT"
    prepare_cache

    local stfile rfile rc=0
    stfile=$(mktemp); rfile=$(mktemp)

    /usr/bin/time -v -o "$stfile" bash -c "$send_cmd" >/dev/null 2>&1 &
    local spid=$!

    sleep 0.5 # let the sender reach listen state

    /usr/bin/time -v -o "$rfile" bash -c "$recv_cmd" >/dev/null 2>&1 || rc=$?
    wait "$spid" 2>/dev/null || true

    if [[ $rc -eq 0 ]] && verify_output; then
        local swall suser ssys rwall ruser rsys sec gbps
        swall=$(grep -F "Elapsed (wall" "$stfile" | awk '{print $NF}' || echo NA)
        suser=$(grep -F "User time"     "$stfile" | awk '{print $NF}' || echo NA)
        ssys=$( grep -F "System time"   "$stfile" | awk '{print $NF}' || echo NA)
        rwall=$(grep -F "Elapsed (wall" "$rfile"  | awk '{print $NF}' || echo NA)
        ruser=$(grep -F "User time"     "$rfile"  | awk '{print $NF}' || echo NA)
        rsys=$( grep -F "System time"   "$rfile"  | awk '{print $NF}' || echo NA)
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
    [[ -x "$EMBR" ]] || die "embr not found at $EMBR — cmake --build $BUILD first"
    command -v /usr/bin/time >/dev/null || die "GNU time missing: sudo dnf install -y time"
    command -v ncat >/dev/null          || die "ncat missing: sudo dnf install -y nmap-ncat"
    [[ -f "$CERT" && -f "$KEY" ]]       || die "QUIC cert/key not found ($CERT, $KEY)"
    assert_toolchain
    assert_sockbuf
    assert_sendzc

    if [[ ! -f "$FILE" ]]; then
        log "generating ${FILE_SIZE_GB}GB random file -> $FILE"
        dd if=/dev/urandom of="$FILE" bs=1M count=$(( FILE_SIZE_GB*1024 )) status=progress
    fi
    EXPECTED_BYTES=$(stat -c%s "$FILE")
    EXPECTED_HASH=$(sha256sum "$FILE" | cut -d' ' -f1)
    log "file $(du -h "$FILE" | cut -f1)  sha256=$EXPECTED_HASH"

    { echo "# bench_quic $(date)"
      echo "# build=$BUILD file=${FILE_SIZE_GB}GB cache=$CACHE_DISCIPLINE"
      echo "# kernel=$(uname -r) memlock=$(ulimit -l)KiB"
      echo "# embrtcp: sendfile/splice, plaintext.  embrquic: mmap Buffer + in-place AEAD + io_uring SEND_ZC egress."
      echo "# embrquicsm: same with EMBR_QUIC_EGRESS=sendmsg (per-datagram copy baseline)."
      echo "# loopback forces ZC_COPIED, so embrquic >= embrquicsm here is expected; the ZC gain is a WAN claim."
      echo "# QUIC is encrypted and TCP is not — compare sys CPU, not wall clock."
      echo ""; } > "$RESULTS"

    local total=$(( RUNS + WARMUP ))
    for (( i=1; i<=total; i++ )); do
        local label="run$i"; (( i <= WARMUP )) && label="warmup$i"
        log "=== $label ($i/$total) ==="

        log "embr tcp..."
        run_transfer "${label}_embrtcp" \
            "$EMBR push $FILE --port $TCP_PORT --transport tcp" \
            "$EMBR pull 127.0.0.1 --port $TCP_PORT --transport tcp --out $OUT" || true

        log "embr quic..."
        run_transfer "${label}_embrquic" \
            "$EMBR push $FILE --port $QUIC_PORT --transport quic --cert $CERT --key $KEY" \
            "$EMBR pull 127.0.0.1 --port $QUIC_PORT --transport quic --out $OUT" || true

        log "embr quic (sendmsg baseline)..."
        run_transfer "${label}_embrquicsm" \
            "EMBR_QUIC_EGRESS=sendmsg $EMBR push $FILE --port $QUIC_PORT --transport quic --cert $CERT --key $KEY" \
            "EMBR_QUIC_EGRESS=sendmsg $EMBR pull 127.0.0.1 --port $QUIC_PORT --transport quic --out $OUT" || true

        log "nc (plaintext reference)..."
        run_transfer "${label}_nc" \
            "ncat -l $NC_PORT --send-only < $FILE" \
            "ncat 127.0.0.1 $NC_PORT --recv-only > $OUT" || true

        echo "" >> "$RESULTS"
    done

    summarize
}

summarize() {
    set +e
    log "=== raw results ==="; cat "$RESULTS" >&2
    log "=== summary (warmup excluded) ==="
    printf '\n%-10s | %-8s | %-10s | %-10s | %-10s | %-10s\n' \
        transport "Gbps" "send_user" "send_sys" "recv_user" "recv_sys"
    printf -- '-----------+----------+------------+------------+------------+-----------\n'
    for tool in embrtcp embrquic embrquicsm nc; do
        local lines gbpss su ss ru rs
        lines=$(grep -E "^run[0-9]+_${tool} " "$RESULTS" || true)
        [[ -z "$lines" ]] && { printf '%-10s | (no data)\n' "$tool"; continue; }
        gbpss=$(sed -n 's/.* gbps=\([^ ]*\).*/\1/p'         <<< "$lines")
        su=$(   sed -n 's/.* send_user=\([0-9.]*\)s.*/\1/p' <<< "$lines")
        ss=$(   sed -n 's/.* send_sys=\([0-9.]*\)s.*/\1/p'  <<< "$lines")
        ru=$(   sed -n 's/.* recv_user=\([0-9.]*\)s.*/\1/p' <<< "$lines")
        rs=$(   sed -n 's/.* recv_sys=\([0-9.]*\)s.*/\1/p'  <<< "$lines")
        printf '%-10s | %-8s | %-10s | %-10s | %-10s | %-10s\n' "$tool" \
            "$(median <<<"$gbpss")" "$(median <<<"$su")" "$(median <<<"$ss")" \
            "$(median <<<"$ru")"    "$(median <<<"$rs")"
    done
    set -e
    log "raw results in $RESULTS"
    log "read: embrquic vs embrquicsm = SEND_ZC vs sendmsg on a copying (loopback) path;"
    log "      embrquic vs embrtcp sys s = kernel work per GiB, encrypted vs plaintext"
}

main "$@"
