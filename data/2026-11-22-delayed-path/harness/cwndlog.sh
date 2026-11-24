#!/bin/bash
# one 256 MiB transfer over the netem path with the sender's ngtcp2 log on; prints what the log says about the window
S=$SCRATCH; E=$1; DELAY=$2
export HOME=$S/home126
mount -t tmpfs tmpfs /run 2>/dev/null; mkdir -p /run/netns
ip netns add r && ip netns add b || exit 1
ip link set lo up; ip link add a0 type veth peer name r0; ip link add r1 type veth peer name b0
ip link set r0 netns r; ip link set r1 netns r; ip link set b0 netns b
ip addr add 10.1.0.1/24 dev a0; ip link set a0 up; ip route add default via 10.1.0.2
R="nsenter --net=/run/netns/r"; B="nsenter --net=/run/netns/b"
$R ip link set lo up; $R ip addr add 10.1.0.2/24 dev r0; $R ip addr add 10.2.0.1/24 dev r1; $R ip link set r0 up; $R ip link set r1 up
$R sysctl -qw net.ipv4.ip_forward=1
$B ip link set lo up; $B ip addr add 10.2.0.2/24 dev b0; $B ip link set b0 up; $B ip route add default via 10.2.0.1
ethtool -K a0 tx-udp-segmentation off >/dev/null 2>&1; $B ethtool -K b0 gro on >/dev/null 2>&1; $R ethtool -K r1 tso off >/dev/null 2>&1
$R tc qdisc add dev r1 root netem delay "$DELAY" rate 2200mbit limit 3000; $R tc qdisc add dev r0 root netem delay "$DELAY" limit 10000
$B ping -c1 -W2 10.1.0.1 >/dev/null || exit 1
head -c 268435456 $S/realperf/f1g.bin > $S/async4/f256.bin
EMBR_QUIC_LOG=1 EMBR_QUIC_GSO=1 EMBR_QUIC_EGRESS=sendmsg EMBR_QUIC_STATS=1 taskset -c 6-9 timeout 120 "$E" push $S/async4/f256.bin --port 17001 --transport quic --cert $HOME/.config/embr/cert.pem --key $HOME/.config/embr/key.pem > /dev/null 2> $S/async4/push-quic.log &
sleep 0.6
start=$(date +%s.%N); $B taskset -c 2,3 "$E" pull 10.1.0.1 --port 17001 --transport quic --out $S/async4/out256.bin > /dev/null 2>&1; end=$(date +%s.%N); wait
echo "delay $DELAY each way: $(awk -v s="$(echo "$end - $start" | bc)" 'BEGIN { printf "%.0f Mbit/s", 268435456 * 8 / s / 1e6 }')"
rm -f $S/async4/out256.bin $S/async4/f256.bin $S/async4/.f256.bin.embr.hash
