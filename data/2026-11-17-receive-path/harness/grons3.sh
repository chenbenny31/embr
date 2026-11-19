#!/bin/bash
# routed A-R-B, device GRO on at b0: which embr sender setting lets the receiver's UDP_GRO coalesce?
S=$SCRATCH
export HOME=$S/home126; CERT=$HOME/.config/embr/cert.pem; KEY=$HOME/.config/embr/key.pem; E=$S/rx-tree/build-Release/embr
mount -t tmpfs tmpfs /run 2>/dev/null; mkdir -p /run/netns
ip netns add r && ip netns add b || exit 1
ip link set lo up; ip link add a0 type veth peer name r0; ip link add r1 type veth peer name b0
ip link set r0 netns r; ip link set r1 netns r; ip link set b0 netns b
ip addr add 10.1.0.1/24 dev a0; ip link set a0 up; ip route add default via 10.1.0.2
R="nsenter --net=/run/netns/r"; B="nsenter --net=/run/netns/b"
$R ip link set lo up; $R ip addr add 10.1.0.2/24 dev r0; $R ip addr add 10.2.0.1/24 dev r1; $R ip link set r0 up; $R ip link set r1 up
$R sysctl -qw net.ipv4.ip_forward=1
$B ip link set lo up; $B ip addr add 10.2.0.2/24 dev b0; $B ip link set b0 up; $B ip route add default via 10.2.0.1
$B ping -c1 -W2 10.1.0.1 >/dev/null || exit 1
ethtool -K a0 tx-udp-segmentation off tso off >/dev/null; $B ethtool -K b0 gro on >/dev/null; $R ethtool -K r1 tso off >/dev/null
cd $S/rxperf; port=22000
em() { # em <desc> <sender env> <receiver env>
      port=$((port+1)); env $2 EMBR_QUIC_STATS=1 timeout 60 $E push $S/realperf/f64.bin --port $port --transport quic --cert $CERT --key $KEY > vd.push.log 2>&1 & sleep 0.5
      t0=$(date +%s.%N); env $3 EMBR_QUIC_STATS=1 timeout 60 $B $E pull 10.1.0.1 --port $port --transport quic --out $S/rxperf/vd.out.bin > vd.pull.log 2>&1; t1=$(date +%s.%N); wait; : > vd.out.bin
      echo "$1: $(echo "$t1 - $t0" | bc)s sender $(grep -ao 'mode=[a-z]* gso=[01] datagrams=[0-9]* sends=[0-9]*' vd.push.log | head -1) | receiver $(grep -ao '\[quic-ingress\].*' vd.pull.log | tail -1 | cut -c16-70)"; }
em "sender SEND_ZC + GSO" "X=1" "X=1"
em "sender sendmsg + GSO" "EMBR_QUIC_EGRESS=sendmsg" "X=1"
em "sender sendmsg + GSO, receiver ACKs by sendmsg" "EMBR_QUIC_EGRESS=sendmsg" "EMBR_QUIC_EGRESS=sendmsg"
em "sender SEND_ZC + GSO, receiver ACKs by sendmsg" "X=1" "EMBR_QUIC_EGRESS=sendmsg"
em "sender sendmsg, GSO off" "EMBR_QUIC_EGRESS=sendmsg EMBR_QUIC_GSO=0" "X=1"
