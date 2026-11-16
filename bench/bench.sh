#!/usr/bin/env bash
# bench.sh — embr transfer benchmarks. Run from the repo root:
#
#   bash bench/bench.sh loopback                                     one host: sender + receiver CPU
#   bash bench/bench.sh wan-sender                                   host A: serves every row until Ctrl-C
#   SENDER_IP=<ip> [SSH_KEY=~/.ssh/key.pem] bash bench/bench.sh wan-receiver
#                                                                    host B: pulls every row, prints the table
#
# Rows — ROWS="..." picks and orders them; use the same ROWS on both WAN hosts:
#   embrtcp        sendfile/splice, plaintext
#   embrquic       ngtcp2 + wolfSSL, mmap Buffer + in-place AEAD, io_uring SEND_ZC egress, one packet per send
#   embrquicsm     same stack, sendmsg egress (per-datagram copy baseline), one packet per send
#   embrquicgso    SEND_ZC egress with UDP GSO: same-size packets batched into one send
#   embrquicsmgso  sendmsg egress with UDP GSO
#   nc             plaintext floor; the fair peer for embr TCP
#   scp            encrypted reference; the fair wall-clock peer for QUIC
# A row's environment is set on the sender only, so the receiver is identical across QUIC rows.
# EMBR_QUIC_ACK_THRESH=N in the caller's environment applies to every QUIC row: the receiver then
# acknowledges every N packets instead of 2 and the sender's GSO batches grow with it. Set it on
# the receiving host (on both for loopback); the results header records it.
#
# Reading it:
#   loopback measures software overhead, not a network. The kernel cannot pin user pages for a
#   loopback receiver, so every SEND_ZC completes as copied: embrquic pays the copy and the
#   notification and lands behind embrquicsm. That is expected; a zero-copy gain is a WAN claim.
#   QUIC is encrypted and TCP is not: compare CPU seconds across them, not wall clock.
#   A process's user/sys leaves out packet work the kernel does in softirq (loopback receive, a
#   NIC's receive and transmit-completion paths). host_cpu/host_irq and, on the WAN receiver,
#   sender_cpu/sender_irq are whole-machine CPU seconds over the transfer and include it.
#
# Both hosts need install.sh to have run (wolfSSL, ngtcp2, liburing, cert, socket limits).
# WAN sender security group: inbound TCP 22, TCP_PORT, NC_PORT and one UDP port per QUIC row
# starting at QUIC_PORT.

set -euo pipefail

MODE="${1:-}"
case "$MODE" in
    loopback)   KIND=loopback; DEF_RUNS=5;  DEF_WARMUP=1; DEF_ROWS="embrtcp embrquic embrquicsm embrquicgso embrquicsmgso nc" ;;
    wan-sender|wan-receiver)
                KIND=wan;      DEF_RUNS=10; DEF_WARMUP=2; DEF_ROWS="embrquic embrquicsm embrquicgso embrquicsmgso embrtcp nc scp" ;;
    *)
        sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'
        echo "Knobs: ROWS RUNS WARMUP BUILD FILE FILE_SIZE_GB CACHE_DISCIPLINE=drop|keep CERT KEY"
        echo "       TCP_PORT QUIC_PORT NC_PORT SENDER_USER (see the top of this script)"
        exit 1 ;;
esac

BUILD="${BUILD:-./build}"
EMBR="${BUILD}/embr"
ROWS="${ROWS:-$DEF_ROWS}"
RUNS="${RUNS:-$DEF_RUNS}"
WARMUP="${WARMUP:-$DEF_WARMUP}"
FILE="${FILE:-/tmp/bench_${KIND}.bin}"
FILE_SIZE_GB="${FILE_SIZE_GB:-1}"
OUT="${OUT:-/tmp/bench_${KIND}_out.bin}"
RESULTS="${RESULTS:-/tmp/bench_${KIND}_results.txt}"
SENDER_STATS="${SENDER_STATS:-/tmp/bench_wan_sender}" # per-push "wall user sys exit" lines, one file per row
CACHE_DISCIPLINE="${CACHE_DISCIPLINE:-drop}"
VERIFY_HASH="${VERIFY_HASH:-1}"
TCP_PORT="${TCP_PORT:-10007}"
QUIC_PORT="${QUIC_PORT:-10008}" # first QUIC row; each further QUIC row takes the next port
NC_PORT="${NC_PORT:-9999}"
CERT="${CERT:-$HOME/.config/embr/cert.pem}" # created by install.sh
KEY="${KEY:-$HOME/.config/embr/key.pem}"
WOLFSSL_OPTS="${WOLFSSL_OPTS:-$HOME/.local/wolfssl/include/wolfssl/options.h}"
SENDER_IP="${SENDER_IP:-}"
SENDER_USER="${SENDER_USER:-ec2-user}"
SSH_KEY="${SSH_KEY:-}"
RETRY_MAX="${RETRY_MAX:-15}"
RETRY_DELAY="${RETRY_DELAY:-1}"
ROUND_GAP="${ROUND_GAP:-2}"

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=10)
[[ -n "$SSH_KEY" ]] && SSH_OPTS+=(-i "$SSH_KEY")
SOCKBUF_MIN=4194304

EXPECTED_BYTES=0
EXPECTED_HASH=""

log() { echo "[bench $(date +%H:%M:%S)] $*" >&2; }
die() { log "FATAL: $*"; exit 1; }

# ── rows ────────────────────────────────────────────────────────────────────────────────────

# every QUIC row, in port order; a new egress variant is one entry here and one in row_env
QUIC_ROWS=(embrquic embrquicsm embrquicgso embrquicsmgso)

is_quic() { [[ "$1" == embrquic* ]]; }
is_embr() { [[ "$1" == embr* ]]; }
any_quic() { local r; for r in $ROWS; do is_quic "$r" && return 0; done; return 1; }
has_row() { local r; for r in $ROWS; do [[ "$r" == "$1" ]] && return 0; done; return 1; }

row_env() { # sender-side environment that defines the row
    case "$1" in
        embrquic)      echo "EMBR_QUIC_GSO=0" ;;
        embrquicsm)    echo "EMBR_QUIC_GSO=0 EMBR_QUIC_EGRESS=sendmsg" ;;
        embrquicgso)   echo "EMBR_QUIC_GSO=1" ;;
        embrquicsmgso) echo "EMBR_QUIC_GSO=1 EMBR_QUIC_EGRESS=sendmsg" ;;
        *)             echo "" ;;
    esac
}

row_port() {
    case "$1" in
        embrtcp) echo "$TCP_PORT" ;;
        nc)      echo "$NC_PORT" ;;
        *)  local i
            for i in "${!QUIC_ROWS[@]}"; do
                [[ "${QUIC_ROWS[$i]}" == "$1" ]] && { echo $(( QUIC_PORT + i )); return 0; }
            done
            die "unknown row: $1" ;;
    esac
}

row_ports() { local r; for r in "${QUIC_ROWS[@]}"; do has_row "$r" && printf '%s ' "$(row_port "$r")"; done; }

check_rows() {
    local r
    for r in $ROWS; do
        case "$r" in
            embrtcp|nc|scp) ;;
            *) is_quic "$r" || die "unknown row: $r (rows: embrtcp ${QUIC_ROWS[*]} nc scp)"
               row_port "$r" >/dev/null ;;
        esac
    done
}

push_args() { # row -> embr push arguments, one per line
    if is_quic "$1"; then
        printf '%s\n' push "$FILE" --port "$(row_port "$1")" --transport quic --cert "$CERT" --key "$KEY"
    else
        printf '%s\n' push "$FILE" --port "$TCP_PORT" --transport tcp
    fi
}

pull_args() { # row host -> embr pull arguments, one per line
    local transport=tcp; is_quic "$1" && transport=quic
    printf '%s\n' pull "$2" --port "$(row_port "$1")" --transport "$transport" --out "$OUT"
}

# ── helpers ─────────────────────────────────────────────────────────────────────────────────

wall_to_sec() {
    awk -F: '{ if(NF==3) print $1*3600+$2*60+$3;
               else if(NF==2) print $1*60+$2; else print $1 }' <<< "$1"
}
median() { sort -n | awk '{a[NR]=$1}
    END{ if(NR==0)print "NA"; else if(NR%2)print a[(NR+1)/2];
         else printf "%.4f\n",(a[NR/2]+a[NR/2+1])/2 }'; }
minmax() { sort -n | awk 'NR==1{mn=$1}{mx=$1}
    END{ if(NR==0)print "NA NA"; else printf "%s %s\n",mn,mx }'; }
field() { sed -n "s/.* $1=\([0-9.]*\).*/\1/p"; } # field gbps < lines

on_sender() { ssh "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}" "$@"; }

# whole-host CPU jiffies as "busy irq": busy = user+nice+system+irq+softirq, irq = irq+softirq.
# Packet work done in softirq (loopback receive, a NIC's receive and transmit-completion paths) is
# charged to no process, so a row's user/sys alone under-reports what the transfer cost the host
CPU_SNAP_AWK='/^cpu /{ print $2+$3+$4+$7+$8, $7+$8; exit }'
cpu_snap() { awk "$CPU_SNAP_AWK" /proc/stat; }
cpu_snap_sender() { on_sender "awk '$CPU_SNAP_AWK' /proc/stat" 2>/dev/null || echo "NA NA"; }
cpu_delta() { # cpu_delta "<busy irq>" "<busy irq>" -> "busy_seconds irq_seconds"
    awk -v a="$1" -v b="$2" -v hz="$(getconf CLK_TCK)" 'BEGIN {
        if (split(a, x, " ") < 2 || split(b, y, " ") < 2 || x[1] == "NA" || y[1] == "NA") { print "NA NA"; exit }
        printf "%.2f %.2f\n", (y[1]-x[1])/hz, (y[2]-x[2])/hz }'
}
host_cpu_table() { # host_cpu_table <key>... : medians of the whole-host fields, one line per row
    local row lines key
    printf '\nwhole-host CPU seconds per transfer (softirq packet work included; irq = irq+softirq part)\n'
    printf '%-13s' row; for key in "$@"; do printf ' | %-10s' "$key"; done; printf '\n'
    for row in $ROWS; do
        lines=$(grep -E "^run[0-9]+_${row} " "$RESULTS") || continue
        printf '%-13s' "$row"
        for key in "$@"; do printf ' | %-10s' "$(field "$key" <<<"$lines" | median)"; done
        printf '\n'
    done
}

prepare_cache() {
    [[ "$CACHE_DISCIPLINE" == "drop" ]] || return 0
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1 || true
    [[ "$MODE" == "wan-receiver" ]] || return 0
    on_sender "sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null" 2>/dev/null || true
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

make_file() {
    if [[ ! -f "$FILE" ]]; then
        log "generating ${FILE_SIZE_GB}GB random file -> $FILE"
        dd if=/dev/urandom of="$FILE" bs=1M count=$(( FILE_SIZE_GB*1024 )) status=progress
    fi
}

results_header() {
    { echo "# bench $MODE $(date)"
      echo "# commit=$(git describe --always --dirty 2>/dev/null || echo unknown) build=$BUILD cache=$CACHE_DISCIPLINE bytes=$EXPECTED_BYTES"
      echo "# rows: $ROWS   ack_thresh=${EMBR_QUIC_ACK_THRESH:-2}"
      echo "$@"
      echo "# embrtcp: sendfile/splice, plaintext.  embrquic: mmap Buffer + in-place AEAD + io_uring SEND_ZC egress."
      echo "# embrquicsm: same with EMBR_QUIC_EGRESS=sendmsg on the sender.  *gso: UDP GSO on (EMBR_QUIC_GSO=1), else off."
      echo "# nc: plaintext floor.  scp: encrypted reference."
      echo ""; } > "$RESULTS"
}

# counters summed over a row's connections: datagrams/sends is the GSO batch size,
# copied/sends the share of sends the kernel copied anyway
egress_summary() { # egress_summary <row> <stderr log of its pushes>
    grep -q '^\[quic-egress\]' "$2" 2>/dev/null || return 0
    awk -v row="$1" '/^\[quic-egress\]/ { n++
             for (i=2;i<=NF;i++) { split($i,kv,"="); if (kv[1] ~ /^(mode|gso|registered)$/) continue
                                   if (!(kv[1] in t)) order[++k]=kv[1]; t[kv[1]]+=kv[2] } }
         END { printf "%s egress over %d connections:", row, n
               for (i=1;i<=k;i++) printf " %s=%d", order[i], t[order[i]]
               if (t["sends"]) printf " (%.1f datagrams per send", t["datagrams"]/t["sends"]
               if (t["sends"] && ("copied" in t)) printf ", copied %.1f%%", 100*t["copied"]/t["sends"]
               if (t["datagrams"] && ("bytes" in t)) printf ", %.0f bytes per datagram", t["bytes"]/t["datagrams"]
               if (t["sends"]) printf ")"
               printf "\n" }' "$2" >&2
}

# ── preflight ───────────────────────────────────────────────────────────────────────────────

assert_binary() {
    [[ -x "$EMBR" ]] || die "embr not found at $EMBR — cmake --build $BUILD first (or set BUILD=)"
    [[ "$BUILD" == *debug* || "$BUILD" == *asan* ]] \
        && die "BUILD=$BUILD is not a release build — never quote a bench number from it"
    command -v /usr/bin/time >/dev/null || die "GNU time missing: sudo dnf install -y time"
    if has_row nc; then command -v ncat >/dev/null || die "ncat missing: sudo dnf install -y nmap-ncat"; fi
    return 0
}

# without --enable-aesgcm-stream wolfSSL memcpys every packet through ctx->authBuffer,
# which silently invalidates every QUIC row
assert_toolchain() {
    [[ -f "$WOLFSSL_OPTS" ]] || die "wolfSSL options.h not found at $WOLFSSL_OPTS — run install.sh"
    grep -q WOLFSSL_AESGCM_STREAM "$WOLFSSL_OPTS" \
        || die "wolfSSL built without --enable-aesgcm-stream — QUIC numbers would be meaningless"
}

assert_cert() {
    [[ -f "$CERT" && -f "$KEY" ]] || die "QUIC cert/key not found ($CERT, $KEY) — run install.sh"
}

# embr asks for 4 MiB UDP buffers; the kernel clamps to rmem_max/wmem_max (212992 on AL2023).
# Below that every chunk becomes a loss event and the number measures a sysctl, not QUIC.
assert_sockbuf() { # assert_sockbuf local|remote
    local where="$1" r w
    if [[ "$where" == "remote" ]]; then
        r=$(on_sender sysctl -n net.core.rmem_max 2>/dev/null || echo 0)
        w=$(on_sender sysctl -n net.core.wmem_max 2>/dev/null || echo 0)
    else
        r=$(sysctl -n net.core.rmem_max); w=$(sysctl -n net.core.wmem_max)
    fi
    (( r >= SOCKBUF_MIN && w >= SOCKBUF_MIN )) \
        || die "$where net.core.rmem_max/wmem_max = $r/$w < $SOCKBUF_MIN — run install.sh there (or: sudo sysctl -w net.core.rmem_max=16777216 net.core.wmem_max=16777216)"
    log "$where sockbuf ok: rmem_max=$r wmem_max=$w"
}

# the SEND_ZC slab registers ~105 KiB of RLIMIT_MEMLOCK per connection; below 1 MiB embr
# silently runs sendmsg and the embrquic row measures nothing new. IORING_SEND_ZC_REPORT_USAGE
# is kernel >= 6.2: older kernels reject every SEND_ZC and the row becomes a copy-fallback run
assert_sendzc() { # assert_sendzc local|remote
    local where="$1" k l
    if [[ "$where" == "remote" ]]; then
        k=$(on_sender uname -r 2>/dev/null || echo 0)
        l=$(on_sender 'ulimit -l' 2>/dev/null || echo 0)
    else
        k=$(uname -r); l=$(ulimit -l)
    fi
    local maj="${k%%.*}" min; min="${k#*.}"; min="${min%%.*}"
    (( maj > 6 || (maj == 6 && min >= 2) )) \
        || die "$where kernel $k < 6.2 — SEND_ZC REPORT_USAGE unsupported; embrquic would be a copy-fallback run"
    [[ "$l" == "unlimited" ]] || (( l >= 1024 )) \
        || die "$where ulimit -l = $l KiB < 1 MiB — the SEND_ZC slab cannot register; embrquic would be a sendmsg run"
    log "$where sendzc ok: kernel=$k memlock=${l}KiB"
}

# fail on the real cause first: every remote preflight would otherwise report a bogus 0
assert_ssh() {
    on_sender true 2>/dev/null \
        || die "cannot ssh ${SENDER_USER}@${SENDER_IP} — check SSH_KEY, key mode 600, and TCP 22 in the sender's security group"
    log "ssh ok: ${SENDER_USER}@${SENDER_IP}"
}

# a closed TCP port would only show as RETRY_MAX slow failures mid-bench; probe before the clock
# starts. UDP cannot be probed this way: the first QUIC warmup is the probe for those ports
assert_ports() {
    local row port
    for row in embrtcp nc; do
        has_row "$row" || continue
        port=$(row_port "$row")
        timeout 5 bash -c "</dev/tcp/$SENDER_IP/$port" 2>/dev/null \
            || die "TCP $port on $SENDER_IP not reachable — sender not started, or port missing from its security group"
        log "tcp port ok: $port ($row)"
    done
}

# ── loopback ────────────────────────────────────────────────────────────────────────────────

# run_pair <label> <row>: sender in the background, receiver in the foreground, both timed
run_pair() {
    local label="$1" row="$2" slog=/dev/null
    local -a send recv
    case "$row" in
        nc)  send=(bash -c "ncat -l $NC_PORT --send-only < '$FILE'")
             recv=(bash -c "ncat 127.0.0.1 $NC_PORT --recv-only > '$OUT'") ;;
        scp) send=(true) # sshd is the sender; its CPU is not charged to a timed process
             recv=(scp -o StrictHostKeyChecking=no "$(whoami)@127.0.0.1:$FILE" "$OUT") ;;
        *)   mapfile -t send < <(push_args "$row"); mapfile -t recv < <(pull_args "$row" 127.0.0.1)
             send=(env $(row_env "$row") EMBR_QUIC_STATS=1 "$EMBR" "${send[@]}"); recv=("$EMBR" "${recv[@]}")
             slog="$RESULTS.$row.log" ;; # the push's [quic-egress] line
    esac

    rm -f "$OUT"
    prepare_cache
    local stfile rfile rc=0 c0 c1
    stfile=$(mktemp); rfile=$(mktemp)

    c0=$(cpu_snap)
    /usr/bin/time -v -o "$stfile" "${send[@]}" >/dev/null 2>>"$slog" &
    local spid=$!
    sleep 0.5 # let the sender reach listen state
    /usr/bin/time -v -o "$rfile" "${recv[@]}" >/dev/null 2>&1 || rc=$?
    wait "$spid" 2>/dev/null || true
    c1=$(cpu_snap) # before verify_output, whose hashing is not the transfer's

    if [[ $rc -eq 0 ]] && verify_output; then
        local suser ssys rwall ruser rsys sec gbps hcpu hirq
        read -r hcpu hirq <<<"$(cpu_delta "$c0" "$c1")"
        suser=$(grep -F "User time"     "$stfile" | awk '{print $NF}' || echo NA)
        ssys=$( grep -F "System time"   "$stfile" | awk '{print $NF}' || echo NA)
        rwall=$(grep -F "Elapsed (wall" "$rfile"  | awk '{print $NF}' || echo NA)
        ruser=$(grep -F "User time"     "$rfile"  | awk '{print $NF}' || echo NA)
        rsys=$( grep -F "System time"   "$rfile"  | awk '{print $NF}' || echo NA)
        sec=$(wall_to_sec "$rwall")
        gbps=$(awk -v b="$EXPECTED_BYTES" -v s="$sec" \
            'BEGIN{ if(s>0)printf "%.2f",b*8/s/1e9; else print "NA" }')
        printf '%s wall=%s sec=%s gbps=%s send_user=%ss send_sys=%ss recv_user=%ss recv_sys=%ss host_cpu=%ss host_irq=%ss\n' \
            "$label" "$rwall" "$sec" "$gbps" "$suser" "$ssys" "$ruser" "$rsys" "$hcpu" "$hirq" | tee -a "$RESULTS" >&2
        rm -f "$stfile" "$rfile"; return 0
    fi
    log "  $label failed (rc=$rc)"
    rm -f "$stfile" "$rfile"; return 1
}

run_loopback() {
    assert_binary
    if any_quic; then assert_toolchain; assert_cert; assert_sockbuf local; assert_sendzc local; fi
    make_file
    EXPECTED_BYTES=$(stat -c%s "$FILE")
    EXPECTED_HASH=$(sha256sum "$FILE" | cut -d' ' -f1)
    log "file $(du -h "$FILE" | cut -f1)  sha256=$EXPECTED_HASH"
    results_header "# kernel=$(uname -r) memlock=$(ulimit -l)KiB — loopback copies every SEND_ZC; compare CPU seconds, not wall"

    local total=$(( RUNS + WARMUP )) i row
    for row in $ROWS; do rm -f "$RESULTS.$row.log"; done
    for (( i=1; i<=total; i++ )); do
        local label="run$i"; (( i <= WARMUP )) && label="warmup$i"
        log "=== $label ($i/$total) ==="
        for row in $ROWS; do run_pair "${label}_${row}" "$row" || true; done
        echo "" >> "$RESULTS"
    done

    set +e
    log "=== raw results ==="; cat "$RESULTS" >&2
    log "=== summary (median, warmup excluded) ==="
    printf '\n%-13s | %-8s | %-8s | %-10s | %-10s | %-10s | %-10s\n' \
        row "Gbps" "wall s" "send_user" "send_sys" "recv_user" "recv_sys"
    printf -- '--------------+----------+----------+------------+------------+------------+-----------\n'
    for row in $ROWS; do
        local lines; lines=$(grep -E "^run[0-9]+_${row} " "$RESULTS")
        [[ -z "$lines" ]] && { printf '%-13s | (no data)\n' "$row"; continue; }
        printf '%-13s | %-8s | %-8s | %-10s | %-10s | %-10s | %-10s\n' "$row" \
            "$(field gbps <<<"$lines" | median)"      "$(field sec <<<"$lines" | median)" \
            "$(field send_user <<<"$lines" | median)" "$(field send_sys <<<"$lines" | median)" \
            "$(field recv_user <<<"$lines" | median)" "$(field recv_sys <<<"$lines" | median)"
    done
    host_cpu_table host_cpu host_irq
    set -e
    echo
    for row in $ROWS; do if is_quic "$row"; then egress_summary "$row" "$RESULTS.$row.log"; fi; done
    log "raw results in $RESULTS"
}

# ── WAN sender ──────────────────────────────────────────────────────────────────────────────

# one transfer per connection, forever; each embr push appends "wall user sys exit" to its row file
serve_row() {
    local row="$1"
    local -a args
    case "$row" in
        scp) return 0 ;; # sshd serves it
        nc)  while true; do ncat -l "$NC_PORT" --send-only < "$FILE" >/dev/null 2>&1 || sleep 0.3; done ;;
        *)   mapfile -t args < <(push_args "$row")
             # EMBR_QUIC_STATS=1: one [quic-egress] line per connection; copied==0 proves real zero-copy
             while true; do
                 env $(row_env "$row") EMBR_QUIC_STATS=1 \
                     /usr/bin/time -f "%e %U %S %x" -a -o "$SENDER_STATS.$row" "$EMBR" "${args[@]}" \
                     >/dev/null 2>>"$SENDER_STATS.$row.log" || sleep 0.3
             done ;;
    esac
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
    printf '\n%-13s | %-6s | %-8s | %-8s\n' row n "user s" "sys s" # wall includes the accept wait, so it is not shown
    local row f lines
    for row in $ROWS; do
        is_embr "$row" || continue
        f="$SENDER_STATS.$row"
        [[ -s "$f" ]] || { printf '%-13s | (no data)\n' "$row"; continue; }
        # a push killed by the exit trap leaves "Command terminated by signal" and a zero line: drop both
        lines=$(awk '/terminated by signal/ { skip=1; next } skip { skip=0; next } NF==4 && $4==0' "$f" | tail -n +$(( WARMUP + 1 )))
        [[ -n "$lines" ]] || { printf '%-13s | (warmup only)\n' "$row"; continue; }
        printf '%-13s | %-6s | %-8s | %-8s\n' "$row" "$(wc -l <<<"$lines")" \
            "$(awk '{print $2}' <<<"$lines" | median)" "$(awk '{print $3}' <<<"$lines" | median)"
    done
    echo
    for row in $ROWS; do if is_quic "$row"; then egress_summary "$row" "$SENDER_STATS.$row.log"; fi; done
    log "raw per-push lines in $SENDER_STATS.<row>; embrquic sys vs embrquicsm sys = SEND_ZC saving per GiB"
}

run_sender() {
    assert_binary
    if any_quic; then assert_toolchain; assert_cert; assert_sockbuf local; assert_sendzc local; fi
    make_file
    log "file $(du -h "$FILE" | cut -f1)  sha256=$(sha256sum "$FILE" | cut -d' ' -f1)"

    local row
    for row in $ROWS; do
        rm -f "$SENDER_STATS.$row" "$SENDER_STATS.$row.log"
        [[ "$row" == scp ]] && continue
        serve_row "$row" &
        SENDER_PIDS+=($!)
    done
    trap stop_sender EXIT INT TERM
    log "serving: $ROWS — tcp:$TCP_PORT nc:$NC_PORT quic udp: $(row_ports)(pids ${SENDER_PIDS[*]})"
    log "Ctrl-C when the receiver is done — the sender summary prints on exit"
    wait
}

# ── WAN receiver ────────────────────────────────────────────────────────────────────────────

run_tool() {
    local label="$1"; shift
    local attempt=0
    while (( attempt < RETRY_MAX )); do
        attempt=$(( attempt + 1 ))
        rm -f "$OUT"; prepare_cache
        local terr ttime rc=0 s0 s1 c0 c1
        terr=$(mktemp); ttime=$(mktemp)
        # the sender's snapshots travel over ssh: its delta carries one ssh login, the same for every row
        s0=$(cpu_snap_sender); c0=$(cpu_snap)
        /usr/bin/time -v -o "$ttime" "$@" >/dev/null 2>"$terr" || rc=$?
        c1=$(cpu_snap); s1=$(cpu_snap_sender)
        if [[ $rc -eq 0 ]] && verify_output; then
            local wall user sys rss sec gbps hcpu hirq scpu sirq
            read -r hcpu hirq <<<"$(cpu_delta "$c0" "$c1")"; read -r scpu sirq <<<"$(cpu_delta "$s0" "$s1")"
            wall=$(grep -F "Elapsed (wall"    "$ttime" | awk '{print $NF}' || echo NA)
            user=$(grep -F "User time"        "$ttime" | awk '{print $NF}' || echo NA)
            sys=$( grep -F "System time"      "$ttime" | awk '{print $NF}' || echo NA)
            rss=$( grep -F "Maximum resident" "$ttime" | awk '{print $NF}' || echo NA)
            sec=$(wall_to_sec "$wall")
            gbps=$(awk -v b="$EXPECTED_BYTES" -v s="$sec" \
                'BEGIN{ if(s>0)printf "%.2f",b*8/s/1e9; else print "NA" }')
            printf '%s wall=%s sec=%s gbps=%s user=%ss sys=%ss rss=%sKB host_cpu=%ss host_irq=%ss sender_cpu=%ss sender_irq=%ss\n' \
                "$label" "$wall" "$sec" "$gbps" "$user" "$sys" "$rss" "$hcpu" "$hirq" "$scpu" "$sirq" | tee -a "$RESULTS" >&2
            rm -f "$terr" "$ttime"; return 0
        fi
        log "  $label attempt $attempt failed (rc=$rc):"
        sed 's/^/      /' "$terr" >&2
        rm -f "$terr" "$ttime"
        sleep "$RETRY_DELAY"
    done
    die "$label failed after $RETRY_MAX attempts"
}

pull_row() { # pull_row <label> <row>
    local label="$1" row="$2"
    local -a args
    case "$row" in
        nc)  run_tool "$label" bash -c "ncat '$SENDER_IP' '$NC_PORT' --recv-only > '$OUT'" ;;
        scp) run_tool "$label" scp "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}:${FILE}" "$OUT" ;;
        *)   mapfile -t args < <(pull_args "$row" "$SENDER_IP")
             run_tool "$label" "$EMBR" "${args[@]}" ;;
    esac
}

run_receiver() {
    [[ -n "$SENDER_IP" ]] || die "set SENDER_IP"
    assert_binary
    assert_ssh
    assert_ports
    if any_quic; then
        assert_toolchain
        assert_sockbuf local; assert_sockbuf remote
        assert_sendzc local;  assert_sendzc remote
    fi

    log "RECEIVER — ${SENDER_USER}@${SENDER_IP} — fetching source size+hash..."
    local meta
    meta=$(on_sender "stat -c%s '$FILE'; sha256sum '$FILE' | cut -d' ' -f1") \
        || die "cannot reach sender — check SSH key and that $FILE exists on sender"
    EXPECTED_BYTES=$(sed -n 1p <<< "$meta")
    EXPECTED_HASH=$(sed -n 2p <<< "$meta")
    [[ "$EXPECTED_BYTES" -gt 0 ]] 2>/dev/null || die "source file empty/missing on sender"
    log "expected $EXPECTED_BYTES bytes  sha256=$EXPECTED_HASH"
    results_header "# sender=$SENDER_IP kernel local=$(uname -r) remote=$(on_sender uname -r 2>/dev/null || echo NA)"

    local total=$(( RUNS + WARMUP )) i row
    for (( i=1; i<=total; i++ )); do
        local label="run$i"; (( i <= WARMUP )) && label="warmup$i"
        log "=== $label ($i/$total) ==="
        for row in $ROWS; do pull_row "${label}_${row}" "$row"; done
        echo "" >> "$RESULTS"
        sleep "$ROUND_GAP"
    done

    set +e
    log "=== raw results ==="; cat "$RESULTS" >&2
    log "=== summary (warmup excluded) — median [min max] ==="
    printf '\n%-13s | %-18s | %-18s | %-8s | %-8s\n' row "throughput Gbps" "wall sec" "user s" "sys s"
    printf -- '--------------+--------------------+--------------------+----------+---------\n'
    for row in $ROWS; do
        local lines; lines=$(grep -E "^run[0-9]+_${row} " "$RESULTS")
        [[ -z "$lines" ]] && { printf '%-13s | (no data)\n' "$row"; continue; }
        printf '%-13s | %-7s [%s] | %-7s [%s] | %-8s | %-8s\n' "$row" \
            "$(field gbps <<<"$lines" | median)" "$(field gbps <<<"$lines" | minmax)" \
            "$(field sec <<<"$lines" | median)"  "$(field sec <<<"$lines" | minmax)"  \
            "$(field user <<<"$lines" | median)" "$(field sys <<<"$lines" | median)"
    done
    host_cpu_table host_cpu host_irq sender_cpu sender_irq
    echo
    set -e
    log "raw results in $RESULTS"
    log "read: user/sys here are the receiver's; the sender's CPU per push prints on the sender at Ctrl-C."
    log "      host_* is this host, sender_* the sender, whole machine: compare rows on these, since"
    log "      softirq packet work is in no process's user/sys"
    log "      embrquic vs embrquicsm = SEND_ZC vs sendmsg; embrquic vs scp = encrypted vs encrypted;"
    log "      embrtcp vs nc = plaintext vs plaintext"
}

check_rows
case "$MODE" in
    loopback)     run_loopback ;;
    wan-sender)   run_sender ;;
    wan-receiver) run_receiver ;;
esac
