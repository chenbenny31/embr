#!/usr/bin/env bash
# wan.sh — the whole two-host WAN run, driven from the receiving host: every pass, both hosts'
# files, one tarball to copy home. Run from the repo root on the receiver:
#
#   SENDER_IP=<ip> SSH_KEY=~/.ssh/key RUNS=1 WARMUP=1 PASSES=default bash bench/wan.sh   smoke test, ~3 minutes
#   SENDER_IP=<ip> SSH_KEY=~/.ssh/key bash bench/wan.sh                                  every pass, ~45 minutes
#
# A pass is one bench.sh wan-sender / wan-receiver run. For each pass this script starts the
# sender over ssh with that pass's environment, runs the receiver here, stops the sender so its
# summary prints, and copies the sender's files back. Nothing is typed on the sender.
#
# Passes — PASSES="..." picks and orders them:
#   default   every row at embr's defaults                              the headline table
#   gro-off   QUIC rows, EMBR_QUIC_GRO=0 on the receiver                what batched receive buys on a real NIC
#   ack10     QUIC rows, EMBR_QUIC_ACK_THRESH=10 on the receiver        whether the ACK rate still matters
#   slots128  SEND_ZC rows, EMBR_QUIC_ZC_SLOTS=128 on the sender        whether the slot pool (3 with GSO) makes the sender wait
#   segs16    GSO rows, EMBR_QUIC_GSO_SEGS=16 on the sender             what 48 packets per send buy over the 16 of before
#   v09-hash  hash on the receiving thread, one chunk at a time         the v0.9 receiver, as the baseline
#   threads-only  verifier threads, one chunk at a time                 what the threads give without side-by-side hashing
#   recv-zc   EMBR_QUIC_EGRESS=zc on the receiver: its ACKs through SEND_ZC, as before short sends were copied
#                                                                       whether its slot pool held a transfer back on a NIC
#   sockbuf-default  both hosts' socket-buffer ceiling at the kernel's default for the pass (needs sudo on both)
#                                                                       what a host without install.sh's sysctl loses
#   custom    CUSTOM='rows|receiver env|sender env'                     a follow-up without editing this file
# embrtcp is in every pass: no pass's environment touches it, so it shows drift between passes.
#
# The run is detached from the terminal: a dropped ssh session does not stop it, and Ctrl-C only
# stops the view. The first lines printed give the log to follow and the pid to kill.
#
# Output, on the receiver:
#   ~/embr-wan/<UTC stamp>/env.{receiver,sender}.txt     kernel, CPU, NIC offloads, limits, build, crypto speed
#   ~/embr-wan/<UTC stamp>/<pass>/results.txt            bench.sh's raw lines, with its .recv.log files
#   ~/embr-wan/<UTC stamp>/<pass>/receiver.terminal.txt  the receiver's whole output, summary tables last
#   ~/embr-wan/<UTC stamp>/<pass>/sender/                per-push lines, [quic-egress] logs, the sender summary
#   ~/embr-wan/<UTC stamp>/<pass>/counters.*.txt         UDP/TCP counters, NIC statistics, interrupts, before and after
#   ~/embr-wan/embr-wan-<UTC stamp>.tar.gz               all of it; the last lines printed say how to fetch it
# Addresses are replaced by SENDER and RECEIVER before the tarball is made, so it can go into
# data/ as it is.
#
# Both hosts: install.sh run, embr built in ./build, the same commit checked out. Sender security
# group: inbound TCP 22, 10007, 9999 and UDP 10008-10012 from the receiver. The key in SSH_KEY
# must be a file on the receiver; a forwarded agent dies with the ssh session.
# The sender host is treated as dedicated: a sender left behind by an aborted run is stopped.
#
# Knobs: PASSES RUNS WARMUP FILE_SIZE_GB BUILD SENDER_USER SENDER_REPO SENDER_BUILD SENDER_ENV
#        OUT_ROOT SENDER_OUT_ROOT SKIP_BUILD=1 DETACH=0, and bench.sh's FILE OUT TCP_PORT QUIC_PORT NC_PORT
#        RETRY_MAX CACHE_DISCIPLINE (RETRY_MAX=2 makes a smoke test fail fast when a port is closed)

set -uo pipefail

export SENDER_IP="${SENDER_IP:-}"
export SENDER_USER="${SENDER_USER:-ec2-user}"
export SSH_KEY="${SSH_KEY:-}"
export RUNS="${RUNS:-10}" WARMUP="${WARMUP:-2}" FILE_SIZE_GB="${FILE_SIZE_GB:-1}" BUILD="${BUILD:-./build}"
PASSES="${PASSES:-default gro-off ack10 slots128}"
if [[ -z "${SENDER_REPO:-}" ]]; then # the same place under the sender's home
    case "$PWD" in "$HOME"/*) SENDER_REPO="~/${PWD#"$HOME"/}" ;; *) SENDER_REPO="$PWD" ;; esac
fi
SENDER_BUILD="${SENDER_BUILD:-./build}"
SENDER_ENV="${SENDER_ENV:-}"                   # extra NAME=value words for every pass's sender
OUT_ROOT="${OUT_ROOT:-$HOME/embr-wan}"
SENDER_OUT_ROOT="${SENDER_OUT_ROOT:-embr-wan}" # under the sender's home unless absolute
SKIP_BUILD="${SKIP_BUILD:-0}"
DETACH="${DETACH:-1}"
STAMP="${STAMP:-$(date -u +%Y-%m-%d-%H%M)}"
RUN_DIR="$OUT_ROOT/$STAMP"
LOG="$OUT_ROOT/$STAMP.log"
SRC_FILE="${FILE:-/tmp/bench_wan.bin}"         # bench.sh's defaults, for the space check
DST_FILE="${OUT:-/tmp/bench_wan_out.bin}"

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes -o ServerAliveInterval=15)
[[ -n "$SSH_KEY" ]] && SSH_OPTS+=(-i "$SSH_KEY")

log() { echo "[wan $(date -u +%H:%M:%S)] $*" >&2; }
die() { log "FATAL: $*"; exit 1; }
on_sender() { ssh "${SSH_OPTS[@]}" "${SENDER_USER}@${SENDER_IP}" "$@"; }

# ── detach ──────────────────────────────────────────────────────────────────────────────────

if [[ -z "${WAN_CHILD:-}" ]]; then
    [[ -f bench/bench.sh ]] || die "run from the repo root"
    [[ -n "$SENDER_IP" ]] || die "set SENDER_IP"
    [[ -e "$RUN_DIR" ]] && die "$RUN_DIR exists — wait a minute or set STAMP"
    for f in "$OUT_ROOT"/*/pid; do # a second run would stop the first one's sender as a leftover
        [[ -f "$f" ]] && grep -qas 'wan\.sh' "/proc/$(cat "$f")/cmdline" \
            && die "another run is in progress (pid $(cat "$f")) — follow it with tail -f ${f%/pid}.log, or kill it first"
    done
    mkdir -p "$RUN_DIR"
    if [[ "$DETACH" == 1 ]]; then
        # started from a subshell that exits at once: as a child of this shell the finished run
        # would stay a zombie, and tail --pid below would wait on it for ever
        ( WAN_CHILD=1 STAMP="$STAMP" nohup setsid bash "$0" > "$LOG" 2>&1 < /dev/null & )
        for _ in $(seq 50); do [[ -s "$RUN_DIR/pid" ]] && break; sleep 0.1; done
        pid=$(cat "$RUN_DIR/pid" 2>/dev/null) || die "the run did not start — see $LOG"
        echo "[wan] running detached as pid $pid. Ctrl-C stops this view, not the run."
        echo "[wan] watch again: tail -f $LOG        stop the run: kill $pid"
        tail --pid="$pid" -n +1 -f "$LOG"
        exit "$(sed -n 's/^\[wan\] exit status \([0-9]*\)$/\1/p' "$LOG" | tail -1 | grep . || echo 1)"
    fi
    export WAN_CHILD=1
    exec > >(trap '' INT TERM; exec tee "$LOG") 2>&1 # tee must outlive a Ctrl-C or a kill to record the cleanup
fi
echo $$ > "$RUN_DIR/pid"

# ── functions that also run on the sender ───────────────────────────────────────────────────

# the remote bash reads these functions, then the call, from stdin: no quoting through ssh
REMOTE_FNS=(r_dir r_repo r_prep r_space r_ports r_sockbuf r_start r_wait r_stop r_clean r_tar snap_env snap_counters)
remote() { { declare -f "${REMOTE_FNS[@]}"; printf '%q ' "$@"; echo; } | on_sender bash -s; }

r_dir()  { case "$1" in /*) echo "$1" ;; *) echo "$HOME/$1" ;; esac; }
r_repo() { case "$1" in "~/"*) echo "$HOME/${1#"~/"}" ;; *) echo "$1" ;; esac; }

r_prep() { # <repo> <build> <skip build>: build what is checked out; prints "<commit> <clean|dirty>"
    cd "$(r_repo "$1")" 2>/dev/null || { echo "no repo at $1" >&2; return 1; }
    if [[ "$3" != 1 ]]; then cmake --build "$2" --target embr >&2 || return 1; fi
    [[ -x "$2/embr" ]] || { echo "no $2/embr in $1" >&2; return 1; }
    echo "$(git rev-parse HEAD) $(git diff --quiet HEAD -- src bench CMakeLists.txt install.sh && echo clean || echo dirty)"
}

r_space() { # <file> <KiB>: room for the test file where it will be written
    [[ -f "$1" ]] && return 0
    local avail; avail=$(df -k --output=avail "$(dirname "$1")" | tail -1)
    (( avail >= $2 )) || { echo "$(dirname "$1") has $(( avail / 1024 )) MiB free, $(( $2 / 1024 )) needed" >&2; return 1; }
}

r_ports() { # <port>...: a push or ncat orphaned by an earlier run would serve the first transfer
    local p busy=""
    for p in "$@"; do [[ -n "$(ss -H -lntu "sport = :$p" 2>/dev/null)" ]] && busy+=" $p"; done
    [[ -z "$busy" ]] || { echo "ports in use on the sender:$busy — stop what holds them (pkill -x embr; pkill -x ncat)" >&2; return 1; }
}

r_sockbuf() { # [<rmem_max> <wmem_max>]: prints this host's ceiling, then sets the one given (needs sudo)
    sysctl -n net.core.rmem_max net.core.wmem_max | tr '\n' ' '
    [[ $# -eq 2 ]] || return 0
    sudo -n sysctl -q -w net.core.rmem_max="$1" net.core.wmem_max="$2"
}

r_start() { # <repo> <dir> NAME=value...: bench.sh wan-sender in a session of its own; its pid is the session id
    local repo dir; repo=$(r_repo "$1"); dir=$(r_dir "$2"); shift 2
    mkdir -p "$dir" && cd "$repo" || return 1
    nohup setsid bash -c 'echo $$ > "$0/sender.pid"; exec env SENDER_STATS="$0/push" "$@" bash bench/bench.sh wan-sender' \
        "$dir" "$@" > "$dir/sender.terminal.txt" 2>&1 < /dev/null &
}

r_wait() { # <dir> <seconds>: 0 once the sender serves; 1 with its last lines if it died or the time ran out
    local dir i pid; dir=$(r_dir "$1")
    for (( i = 0; i < $2 * 2; i++ )); do
        grep -q 'serving:' "$dir/sender.terminal.txt" 2>/dev/null && return 0
        pid=$(cat "$dir/sender.pid" 2>/dev/null)
        if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then break; fi
        sleep 0.5
    done
    tail -n 20 "$dir/sender.terminal.txt" >&2
    return 1
}

r_stop() { # <dir>: TERM lets bench.sh print its sender summary; then whatever is left in its session
    local dir pid i; dir=$(r_dir "$1")
    pid=$(cat "$dir/sender.pid" 2>/dev/null) || return 0
    grep -qas wan-sender "/proc/$pid/cmdline" || return 0
    kill -TERM "$pid" 2>/dev/null
    for (( i = 0; i < 100; i++ )); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    pkill -KILL -s "$pid" 2>/dev/null
    return 0
}

r_clean() { # <root>: a sender left by an aborted run would hold the ports
    local root f pid; root=$(r_dir "$1")
    for f in "$root"/*/*/sender.pid; do
        [[ -f "$f" ]] || continue
        pid=$(cat "$f")
        grep -qas wan-sender "/proc/$pid/cmdline" || continue
        kill -TERM "$pid" 2>/dev/null; sleep 1; pkill -KILL -s "$pid" 2>/dev/null
        echo "stopped a sender left by an earlier run (pid $pid)" >&2
    done
    # the whole command line, so that an editor or a shell that only mentions the script is not taken for one
    pid=$(pgrep -fx '(.*/)?bash (.*/)?bench\.sh wan-sender' | head -1)
    [[ -z "$pid" ]] || { echo "a wan-sender started by hand is running on the sender: stop it there first (kill $pid)" >&2; return 1; }
}

r_tar() { tar -C "$(r_dir "$1")" -cf - .; }

# what the numbers were measured on. No addresses: this file goes into a public repository.
snap_env() { # <peer ip, or "" for the ssh client> <repo> <build> <test file> <measure rtt: 0|1>
    local peer="$1" repo build="$3" file="$4" dev token i t0 t1 lib path
    repo=$(r_repo "$2")
    [[ "$build" == /* ]] || build="$repo/$build"
    [[ -n "$peer" ]] || peer="${SSH_CLIENT%% *}"
    dev=$(ip -o route get "$peer" 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p')

    echo "date: $(date -u +%FT%TZ)"
    echo "kernel: $(uname -srm)"
    echo "os: $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
    if grep -qis amazon /sys/devices/virtual/dmi/id/sys_vendor; then
        token=$(curl -s -m 2 -X PUT http://169.254.169.254/latest/api/token \
                     -H 'X-aws-ec2-metadata-token-ttl-seconds: 60' 2>/dev/null) || token=""
        echo "instance: $(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null) $(curl -s -m 2 \
            -H "X-aws-ec2-metadata-token: $token" http://169.254.169.254/latest/meta-data/placement/availability-zone 2>/dev/null)"
    fi
    echo "cpu: $(lscpu | sed -n 's/^Model name: *//p' | head -1), $(nproc) cpus, $(lscpu | sed -n 's/^Thread(s) per core: *//p') threads per core"
    echo "cpu flags of interest: $(grep -m1 '^flags' /proc/cpuinfo | tr ' ' '\n' | grep -x -E 'aes|vaes|pclmulqdq|vpclmulqdq|avx2|avx512f|sha_ni' | tr '\n' ' ')"
    echo "memory: $(free -m | awk '/^Mem:/ { print $2 " MiB total, " $7 " MiB available" }')"
    echo "load: $(cut -d' ' -f1-3 /proc/loadavg)"
    echo "memlock: $(ulimit -l) KiB"
    echo "sysctl: $(sysctl -n net.core.rmem_max net.core.wmem_max net.core.rmem_default net.core.wmem_default 2>/dev/null | tr '\n' ' ')(rmem_max wmem_max rmem_default wmem_default)"
    echo "sysctl: tcp_congestion_control=$(sysctl -n net.ipv4.tcp_congestion_control 2>/dev/null) default_qdisc=$(sysctl -n net.core.default_qdisc 2>/dev/null) netdev_max_backlog=$(sysctl -n net.core.netdev_max_backlog 2>/dev/null) netdev_budget=$(sysctl -n net.core.netdev_budget 2>/dev/null)"
    echo "sysctl: tcp_rmem=$(sysctl -n net.ipv4.tcp_rmem 2>/dev/null | tr '\t' ' ') tcp_wmem=$(sysctl -n net.ipv4.tcp_wmem 2>/dev/null | tr '\t' ' ') udp_mem=$(sysctl -n net.ipv4.udp_mem 2>/dev/null | tr '\t' ' ')"

    echo "test file directory: $(findmnt -n -o FSTYPE,SIZE,AVAIL -T "$(dirname "$file")" 2>/dev/null) (fstype size avail; tmpfs means the file is in memory)"

    echo "repo: $(git -C "$repo" describe --always --dirty 2>/dev/null) $(git -C "$repo" log -1 --format='%H %cI' 2>/dev/null)"
    echo "build: $(grep -s '^CMAKE_BUILD_TYPE' "$build/CMakeCache.txt") $(c++ --version 2>/dev/null | head -1)"
    ldd "$build/embr" 2>/dev/null | awk '/ngtcp2|wolfssl|uring|crypto/ { print $1, $3 }' | while read -r lib path; do
        echo "library: $lib -> $(basename "$(readlink -f "$path")")"
    done
    echo "installed: ngtcp2 $(PKG_CONFIG_PATH="$HOME/.local/ngtcp2/lib/pkgconfig" pkg-config --modversion libngtcp2 2>/dev/null), wolfssl $(PKG_CONFIG_PATH="$HOME/.local/wolfssl/lib/pkgconfig" pkg-config --modversion wolfssl 2>/dev/null), liburing $(pkg-config --modversion liburing 2>/dev/null)"
    for i in sha256 aes-128-gcm aes-256-gcm; do # one core's ceiling for the hash and the AEAD (OpenSSL's code, not wolfSSL's)
        echo "openssl speed $i, 16 KiB blocks: $(openssl speed -seconds 1 -bytes 16384 -evp "$i" 2>/dev/null | tail -1 | awk '{ print $NF }')"
    done

    if [[ "$5" == 1 ]]; then # one TCP connect is one round trip plus about 1 ms of process start
        printf 'tcp connect to the peer port 22, ms:'
        for i in 1 2 3 4 5; do
            t0=$(date +%s%N); timeout 3 bash -c "</dev/tcp/$peer/22" 2>/dev/null; t1=$(date +%s%N)
            printf ' %d.%d' $(( (t1 - t0) / 1000000 )) $(( (t1 - t0) / 100000 % 10 ))
        done
        printf '\nping: %s\n' "$(ping -c 5 -i 0.2 -q -W 2 "$peer" 2>/dev/null | tail -1 || true)"
    fi

    echo; echo "## device $dev: mtu $(cat "/sys/class/net/$dev/mtu" 2>/dev/null), $(tc qdisc show dev "$dev" 2>/dev/null | head -4 | tr '\n' ';')"
    if command -v ethtool >/dev/null; then
        ethtool -i "$dev" 2>/dev/null | grep -E '^(driver|version|firmware-version):'
        echo "## ethtool -k $dev"; ethtool -k "$dev" 2>/dev/null | grep -v -e '^Features' -e 'off \[fixed\]$'
        echo "## ethtool -k $dev, fixed off"; ethtool -k "$dev" 2>/dev/null | sed -n 's/: off \[fixed\]$//p' | tr '\n' ' ' | tr -d '\t'; echo
        echo "## ethtool -l $dev"; ethtool -l "$dev" 2>/dev/null
        echo "## ethtool -c $dev"; ethtool -c "$dev" 2>/dev/null | grep -E 'Adaptive|^(rx|tx)-(usecs|frames):'
        echo "## ethtool -g $dev"; ethtool -g "$dev" 2>/dev/null
    else
        echo "ethtool missing: sudo dnf install -y ethtool"
    fi
}

snap_counters() { # <peer ip, or "" for the ssh client>: taken before and after a pass
    local peer="$1" dev
    [[ -n "$peer" ]] || peer="${SSH_CLIENT%% *}"
    dev=$(ip -o route get "$peer" 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p')
    echo "# $(date -u +%FT%TZ) dev=$dev load=$(cut -d' ' -f1-3 /proc/loadavg)"
    echo "## /proc/net/snmp";    grep -E '^(Ip|Udp|Tcp):' /proc/net/snmp
    echo "## /proc/net/netstat"; grep -E '^(TcpExt|IpExt):' /proc/net/netstat
    echo "## ethtool -S $dev";   ethtool -S "$dev" 2>/dev/null
    echo "## ip -s link show dev $dev"; ip -s link show dev "$dev" 2>/dev/null | grep -v -e 'link/' -e 'altname'
    echo "## /proc/net/softnet_stat";   cat /proc/net/softnet_stat
    echo "## /proc/interrupts, $dev";   head -1 /proc/interrupts; grep -i -e "${dev:-none}" -e ena -e virtio /proc/interrupts
    echo "## /proc/softirqs";           grep -E 'CPU|NET_RX|NET_TX' /proc/softirqs
}

# ── here only ───────────────────────────────────────────────────────────────────────────────

QUIC_ROWS="embrquic embrquicsm embrquicgso embrquicsmgso embrquicdef"
pass_spec() { # <name> -> rows|receiver env|sender env[|socket-buffer ceiling]
    case "$1" in
        default)  echo "$QUIC_ROWS embrtcp nc scp||" ;;
        gro-off)  echo "$QUIC_ROWS embrtcp|EMBR_QUIC_GRO=0|" ;;
        ack10)    echo "$QUIC_ROWS embrtcp|EMBR_QUIC_ACK_THRESH=10|" ;;
        slots128) echo "embrquic embrquicgso embrquicsmgso embrtcp||EMBR_QUIC_ZC_SLOTS=128" ;;
        segs16)   echo "embrquicgso embrquicsmgso embrquicdef embrtcp||EMBR_QUIC_GSO_SEGS=16" ;;
        v09-hash)     echo "$QUIC_ROWS embrtcp|EMBR_VERIFY=inline EMBR_HASH_LANES=1|" ;;
        threads-only) echo "$QUIC_ROWS embrtcp|EMBR_HASH_LANES=1|" ;;
        recv-zc)      echo "$QUIC_ROWS embrtcp|EMBR_QUIC_EGRESS=zc|" ;;
        # fourth field: the socket-buffer ceiling set on both hosts for the pass (212992 is the kernel's default)
        sockbuf-default) echo "$QUIC_ROWS embrtcp|SOCKBUF_MIN=0|SOCKBUF_MIN=0|212992" ;;
        custom)   [[ "${CUSTOM:-}" == *"|"*"|"* ]] && echo "$CUSTOM" ;;
        *)        return 1 ;;
    esac
}

kill_tree() {
    local p c
    for p in "$@"; do
        for c in $(pgrep -P "$p" 2>/dev/null); do kill_tree "$c"; done
        kill "$p" 2>/dev/null || true
    done
}

# "<name>+<increase>" for the counters that say a host or the instance limits dropped traffic
udp_delta() { # <before> <after>
    awk '/^Udp: [A-Za-z]/ { for (i = 2; i <= NF; i++) name[i] = $i; next }
         /^Udp: [0-9]/    { f++; for (i = 2; i <= NF; i++) v[f, i] = $i }
         END { for (i = 2; i in name; i++)
                   if (name[i] ~ /^(InDatagrams|OutDatagrams|InErrors|RcvbufErrors|SndbufErrors)$/)
                       printf " %s+%d", name[i], v[2, i] - v[1, i] }' "$1" "$2"
}
nic_delta() { # <before> <after>: ENA's allowance counters, and any drop or trouble counter that moved
    awk 'FNR == NR { if ($1 ~ /allowance_exceeded|drop|queue_stop|csum_bad|bad_csum|linearize|missed_tx|alloc_fail|empty_rx_ring/) a[$1] = $2; next }
         ($1 in a) && ($1 ~ /allowance_exceeded/ || $2 != a[$1]) { sub(/:$/, "", $1); printf " %s+%d", $1, $2 - a[$1 ":"] }' "$1" "$2"
}

RECV_PID=""
SENDER_UP="" # the pass whose sender is running
SOCKBUF_LOCAL="" SOCKBUF_REMOTE="" # ceilings to put back after a pass that changed them
restore_sockbuf() {
    # shellcheck disable=SC2086  # "rmem wmem", split on purpose
    [[ -z "$SOCKBUF_LOCAL" ]]  || r_sockbuf $SOCKBUF_LOCAL > /dev/null || log "WARNING: the receiver's socket-buffer ceiling was not put back ($SOCKBUF_LOCAL)"
    # shellcheck disable=SC2086
    [[ -z "$SOCKBUF_REMOTE" ]] || remote r_sockbuf $SOCKBUF_REMOTE > /dev/null || log "WARNING: the sender's socket-buffer ceiling was not put back ($SOCKBUF_REMOTE)"
    SOCKBUF_LOCAL=""; SOCKBUF_REMOTE=""
}

stop_local() { # whatever this run still has running on this host
    if [[ "$(ps -o sid= -p $$ | tr -d ' ')" == "$$" ]]; then
        pkill -TERM -s $$ 2>/dev/null # detached: the session is this run's alone, and this shell ignores TERM by now
    elif [[ -n "$RECV_PID" ]]; then
        kill_tree "$RECV_PID"
    fi
}

stop_fetch() { # <pass>: stop its sender and bring the sender's files here
    local name="$1" dir="$RUN_DIR/$1" f
    remote r_stop "$SENDER_OUT_ROOT/$STAMP/$name"
    SENDER_UP=""
    remote r_tar "$SENDER_OUT_ROOT/$STAMP/$name" | tar -C "$dir/sender" -xf - \
        || log "could not copy the sender's files for $name: they stay on the sender in $SENDER_OUT_ROOT/$STAMP/$name"
    rm -f "$dir/sender/sender.pid"
    for f in "$dir"/sender/push.*; do # per-push lines have no extension; data/ takes .txt and .log
        [[ -f "$f" ]] || continue
        case "$f" in *.log|*.txt) ;; *) mv "$f" "$f.txt" ;; esac
    done
    find "$dir" -type f -empty -delete
}

run_pass() { # <name>
    local name="$1" spec rows renv senv sockbuf rc=0 v
    local dir="$RUN_DIR/$1" rdir="$SENDER_OUT_ROOT/$STAMP/$1"
    spec=$(pass_spec "$name") || { log "unknown pass: $name (custom needs CUSTOM='rows|receiver env|sender env')"; return 1; }
    IFS='|' read -r rows renv senv sockbuf <<<"$spec"
    mkdir -p "$dir/sender"
    log "=== pass $name: rows [$rows]  receiver env [${renv:-none}]  sender env [${senv:-none}] ==="
    { echo "pass: $name"; echo "rows: $rows"
      echo "receiver env: ${renv:-none}"; echo "sender env: ${senv:-none}${SENDER_ENV:+ $SENDER_ENV}"
      echo "runs: $RUNS  warmup: $WARMUP  file_size_gb: $FILE_SIZE_GB"; echo "started: $(date -u +%FT%TZ)"; } > "$dir/pass.txt"

    if [[ -n "$sockbuf" ]]; then # both hosts, for this pass only
        if ! SOCKBUF_LOCAL=$(r_sockbuf "$sockbuf" "$sockbuf") || ! SOCKBUF_REMOTE=$(remote r_sockbuf "$sockbuf" "$sockbuf"); then
            restore_sockbuf
            log "pass $name: cannot set the socket-buffer ceiling — it needs sudo without a password on both hosts"
            return 1
        fi
        echo "socket-buffer ceiling: $sockbuf on both hosts (receiver had $SOCKBUF_LOCAL, sender had $SOCKBUF_REMOTE)" >> "$dir/pass.txt"
    fi

    local -a sender_env=("ROWS=$rows" "WARMUP=$WARMUP" "FILE_SIZE_GB=$FILE_SIZE_GB" "BUILD=$SENDER_BUILD")
    for v in FILE TCP_PORT QUIC_PORT NC_PORT; do [[ -n "${!v:-}" ]] && sender_env+=("$v=${!v}"); done
    # shellcheck disable=SC2206  # NAME=value words, split on purpose
    sender_env+=($senv $SENDER_ENV)

    remote r_start "$SENDER_REPO" "$rdir" "${sender_env[@]}" || { restore_sockbuf; log "pass $name: could not start the sender"; return 1; }
    SENDER_UP="$name"
    # the first pass generates and hashes the test file before it serves
    remote r_wait "$rdir" 300 || { log "pass $name: the sender did not come up"; stop_fetch "$name"; restore_sockbuf; return 1; }

    snap_counters "$SENDER_IP" > "$dir/counters.receiver.before.txt"
    remote snap_counters ""    > "$dir/counters.sender.before.txt"

    # in the background so that a kill of this script is not held up until the pass ends
    # shellcheck disable=SC2086
    ( set -o pipefail
      env $renv ROWS="$rows" RESULTS="$dir/results.txt" bash bench/bench.sh wan-receiver 2>&1 | tee "$dir/receiver.terminal.txt" ) &
    RECV_PID=$!
    wait "$RECV_PID"; rc=$?
    RECV_PID=""

    snap_counters "$SENDER_IP" > "$dir/counters.receiver.after.txt"
    remote snap_counters ""    > "$dir/counters.sender.after.txt"
    stop_fetch "$name"
    restore_sockbuf

    for v in receiver sender; do
        log "pass $name, $v: udp$(udp_delta "$dir/counters.$v.before.txt" "$dir/counters.$v.after.txt")  nic$(nic_delta "$dir/counters.$v.before.txt" "$dir/counters.$v.after.txt")"
    done 2>&1 | tee "$dir/counter-deltas.txt"
    echo "finished: $(date -u +%FT%TZ)  receiver exit status: $rc" >> "$dir/pass.txt"
    (( rc == 0 )) || log "pass $name FAILED (receiver exit status $rc) — the next pass still runs"
    return "$rc"
}

preflight() {
    local here there need
    [[ -x "$BUILD/embr" ]] || die "no $BUILD/embr here — BUILD_EMBR=1 bash install.sh"
    on_sender true 2>/dev/null \
        || die "cannot ssh ${SENDER_USER}@${SENDER_IP} — SSH_KEY must be a key file on this host (mode 600) and TCP 22 open on the sender"
    remote r_clean "$SENDER_OUT_ROOT" || die "the sender is not free"
    local q="${QUIC_PORT:-10008}"
    remote r_ports "${TCP_PORT:-10007}" "${NC_PORT:-9999}" "$q" $(( q + 1 )) $(( q + 2 )) $(( q + 3 )) $(( q + 4 )) || die "the sender is not free"

    # a pull without a rebuild would measure the old binary
    if [[ "$SKIP_BUILD" != 1 ]]; then cmake --build "$BUILD" --target embr >&2 || die "build failed here"; fi
    here="$(git rev-parse HEAD) $(git diff --quiet HEAD -- src bench CMakeLists.txt install.sh && echo clean || echo dirty)"
    there=$(remote r_prep "$SENDER_REPO" "$SENDER_BUILD" "$SKIP_BUILD") \
        || die "sender not ready: repo expected at $SENDER_REPO (set SENDER_REPO), built in $SENDER_BUILD"
    [[ "${here%% *}" == "${there%% *}" ]] \
        || die "different commits: ${here:0:7} here, ${there:0:7} on the sender — git pull on both, then run again"
    [[ "$here" == *clean && "$there" == *clean ]] || log "WARNING: uncommitted changes (here: ${here##* }, sender: ${there##* }) — the results header will say -dirty"
    log "commit ${here:0:7} on both hosts, built"

    need=$(( FILE_SIZE_GB * 1024 * 1024 + 262144 ))
    r_space "$DST_FILE" "$need" || die "no room here for the received file"
    remote r_space "$SRC_FILE" "$need"  || die "no room on the sender for the test file"

    RECEIVER_IP=$(on_sender 'echo ${SSH_CLIENT%% *}' 2>/dev/null) || RECEIVER_IP=""
}

recap() {
    local name d
    log "=== recap: each pass's summary as bench.sh printed it ==="
    for name in $PASSES; do
        d="$RUN_DIR/$name"
        [[ -f "$d/receiver.terminal.txt" ]] || continue
        echo; echo "######## pass $name — $(sed -n '3,4p' "$d/pass.txt" | tr '\n' ' ')"
        sed -n '/=== summary/,$p' "$d/receiver.terminal.txt" | grep -v -e 'raw results in' -e '\] read:' -e '\]       '
        sed -n '/=== sender summary/,$p' "$d/sender/sender.terminal.txt" 2>/dev/null | grep -v 'raw per-push lines'
        cat "$d/counter-deltas.txt" 2>/dev/null
    done
}

scrub() { # addresses out of everything that will be committed
    local pair ip f
    for pair in "$SENDER_IP=SENDER" "${RECEIVER_IP:-}=RECEIVER"; do
        ip="${pair%%=*}"
        [[ -n "$ip" ]] || continue
        grep -rlIF -- "$ip" "$RUN_DIR" 2>/dev/null | while read -r f; do sed -i "s/${ip//./\\.}/${pair#*=}/g" "$f"; done
    done
}

FAILED=""
STOPPED=""
finish() {
    trap - EXIT
    trap '' TERM INT
    stop_local
    [[ -n "$SENDER_UP" ]] && stop_fetch "$SENDER_UP"
    restore_sockbuf
    rm -f "$RUN_DIR/pid"
    if ! ls "$RUN_DIR"/*/pass.txt >/dev/null 2>&1; then
        rmdir "$RUN_DIR" 2>/dev/null
        log "nothing was measured"
        echo "[wan] exit status 1"
        exit 1
    fi
    recap
    log "passes run: $PASSES${FAILED:+ — FAILED:$FAILED}"
    sleep 0.3 # let tee write the lines above
    cp "$LOG" "$RUN_DIR/terminal.txt" 2>/dev/null
    scrub
    local tarball="$OUT_ROOT/embr-wan-$STAMP.tar.gz"
    tar -C "$OUT_ROOT" -czf "$tarball" "$STAMP"
    log "all files: $RUN_DIR"
    log "tarball:   $tarball ($(du -h "$tarball" | cut -f1)) — copy it home before the instances are terminated."
    log "On your desktop, in the embr repo:"
    log "  scp -i <key.pem> $(whoami)@${RECEIVER_IP:-<receiver ip>}:${tarball#"$HOME"/} /tmp/"
    log "  mkdir -p data/${STAMP%-*}-wan && tar -xzf /tmp/embr-wan-$STAMP.tar.gz -C data/${STAMP%-*}-wan --strip-components=1"
    local rc=0; [[ -z "$FAILED" && -z "$STOPPED" ]] || rc=1
    echo "[wan] exit status $rc"
    exit "$rc"
}

trap finish EXIT
trap 'log "stopped by a signal"; STOPPED=1; exit 143' TERM INT

log "run $STAMP: passes [$PASSES]  runs=$RUNS warmup=$WARMUP file=${FILE_SIZE_GB}GiB  sender ${SENDER_USER}@<sender>:$SENDER_REPO"
preflight
{ echo "run: $STAMP"; echo "passes: $PASSES"; echo "runs: $RUNS  warmup: $WARMUP  file_size_gb: $FILE_SIZE_GB"
  echo "commit: $(git describe --always --dirty)"; } > "$RUN_DIR/run.txt"
snap_env "$SENDER_IP" "$PWD" "$BUILD" "$DST_FILE" 1                 > "$RUN_DIR/env.receiver.txt" 2>&1
remote snap_env "" "$SENDER_REPO" "$SENDER_BUILD" "$SRC_FILE" 0     > "$RUN_DIR/env.sender.txt" 2>&1
log "receiver: $(sed -n 's/^kernel: //p;s/^instance: //p;s/^cpu: //p' "$RUN_DIR/env.receiver.txt" | tr '\n' ';')"
log "sender:   $(sed -n 's/^kernel: //p;s/^instance: //p;s/^cpu: //p' "$RUN_DIR/env.sender.txt" | tr '\n' ';')"
log "round trip: $(sed -n 's/^tcp connect to the peer port 22, ms: //p' "$RUN_DIR/env.receiver.txt") ms (tcp connect)"

for pass in $PASSES; do
    run_pass "$pass" || FAILED+=" $pass"
done
