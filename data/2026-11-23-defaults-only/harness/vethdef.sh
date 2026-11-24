#!/bin/bash
# Both ends at their defaults on a WAN-like path: sender A -- router R -- receiver B over veth, the data
# direction rate-limited and both directions delayed by netem. Packets cross one by one, as on a wire.
# Adapted from data/2026-11-17-receive-path/harness/veth.sh; adds CPU pinning and the receiver's
# UDP receive-buffer drops per transfer.
#
#   SCRATCH=<dir> unshare --user --map-root-user --net --mount bash vethsb.sh <rate> <delay each way> <limit> <file> <runs> <label>=<embr binary>[,ENV=VAL...] ...
# ENV=VAL applies to both ends. RECV_CPUS (default 2,3) pins the pull, the push runs on CPUs 6-9.
S=${SCRATCH:?scratch directory holding a home with the QUIC cert}
RATE=$1; DELAY=$2; LIMIT=$3; FILE=$4; RUNS=$5; shift 5
RECV_CPUS=${RECV_CPUS:-2,3}
export HOME=$S/home126
CERT=$HOME/.config/embr/cert.pem; KEY=$HOME/.config/embr/key.pem
mount -t tmpfs tmpfs /run 2>/dev/null; mkdir -p /run/netns
ip netns add r && ip netns add b || { echo "netns setup failed"; exit 1; }
ip link set lo up; ip link add a0 type veth peer name r0; ip link add r1 type veth peer name b0
ip link set r0 netns r; ip link set r1 netns r; ip link set b0 netns b
ip addr add 10.1.0.1/24 dev a0; ip link set a0 up; ip route add default via 10.1.0.2
R="nsenter --net=/run/netns/r"; B="nsenter --net=/run/netns/b"
$R ip link set lo up; $R ip addr add 10.1.0.2/24 dev r0; $R ip addr add 10.2.0.1/24 dev r1; $R ip link set r0 up; $R ip link set r1 up
$R sysctl -qw net.ipv4.ip_forward=1
$B ip link set lo up; $B ip addr add 10.2.0.2/24 dev b0; $B ip link set b0 up; $B ip route add default via 10.2.0.1
ethtool -K a0 tx-udp-segmentation off >/dev/null 2>&1 || echo "could not switch UDP segmentation offload off on a0"
$B ethtool -K b0 gro on >/dev/null 2>&1 || echo "could not switch GRO on on b0"
$R ethtool -K r1 tso off >/dev/null 2>&1 || echo "could not switch TSO off on r1"
$R tc qdisc add dev r1 root netem delay "$DELAY" rate "$RATE" limit "$LIMIT" || { echo "netem failed"; exit 1; }  # data direction
$R tc qdisc add dev r0 root netem delay "$DELAY" limit 10000                                                      # ACK direction
$B ping -c1 -W2 10.1.0.1 >/dev/null || { echo "no route"; exit 1; }
echo "# veth A-R-B  rate=$RATE delay=$DELAY each way limit=$LIMIT packets  file=$(stat -c%s "$FILE") bytes  receiver CPUs $RECV_CPUS  kernel=$(uname -r)  $($B ping -c3 -i0.2 -q 10.1.0.1 | tail -1)"
drops() { $B awk '/^Udp:/ { if (!col) { for (i = 2; i <= NF; i++) if ($i == "RcvbufErrors") col = i } else { print $col; exit } }' /proc/net/snmp; }
bytes=$(stat -c%s "$FILE"); mkdir -p "$S/async4/veth"; cd "$S/async4/veth" || exit 1; port=17000
for run in $(seq 1 "$RUNS"); do
  for spec in "$@"; do
    label=${spec%%=*}; rest=${spec#*=}; embr=${rest%%,*}; envs=$(echo "${rest#"$embr"}" | tr ',' ' ')
    port=$((port + 1)); before=$(drops)
    # shellcheck disable=SC2086
    env $envs EMBR_QUIC_STATS=1 taskset -c 6-9 timeout 300 "$embr" push "$FILE" --port $port --transport quic --cert "$CERT" --key "$KEY" > push.log 2>&1 &
    sleep 0.6
    start=$(date +%s.%N)
    # shellcheck disable=SC2086
    env $envs EMBR_QUIC_STATS=1 timeout 300 $B taskset -c "$RECV_CPUS" "$embr" pull 10.1.0.1 --port $port --transport quic --out "$S/async4/veth/out.bin" > pull.log 2>&1; rc=$?
    end=$(date +%s.%N)
    wait
    ok=MISMATCH; cmp -s "$FILE" out.bin && ok=intact; : > out.bin
    echo "run$run $label rc=$rc $ok mbps=$(awk -v b="$bytes" -v s="$(echo "$end - $start" | bc)" 'BEGIN { printf "%.0f", b * 8 / s / 1e6 }') drops=$(( $(drops) - before )) sender_$(grep -ao 'datagrams=[0-9]*' push.log | head -1) receiver_$(grep -ao 'datagrams=[0-9]* reads=[0-9]*' pull.log | tail -1 | tr ' ' '_') push_egress: $(grep -ao '\[quic-egress\].*' push.log | tail -1 | grep -o 'sends=[0-9]*\|copied=[0-9]*\|waits=[0-9]*\|by_sendmsg=[0-9]*' | tr '\n' ' ')"
  done
done
$R tc -s qdisc show dev r1 2>/dev/null | grep -E 'dropped' | head -1 | sed 's/^ */# r1 netem: /'
