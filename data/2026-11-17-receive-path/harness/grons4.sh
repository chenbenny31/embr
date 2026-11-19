#!/bin/bash
# routed A-R-B: look at the packets on the wire (r1, towards B) for a SEND_ZC sender and a sendmsg sender
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
cd $S/rxperf; port=23000
cap() { # cap <desc> <sender env>
      port=$((port+1))
      $R timeout 20 tcpdump -Z root -i r1 -n -v -c 3000 "udp and src host 10.1.0.1" > cap.$port.txt 2> cap.$port.err &
      sleep 0.5
      env $2 EMBR_QUIC_STATS=1 timeout 60 $E push $S/rxperf/f5.bin --port $port --transport quic --cert $CERT --key $KEY > vd.push.log 2>&1 & sleep 0.5
      EMBR_QUIC_STATS=1 timeout 60 $B $E pull 10.1.0.1 --port $port --transport quic --out $S/rxperf/vd.out.bin > vd.pull.log 2>&1; wait; : > vd.out.bin
      echo "== $1: receiver $(grep -ao '\[quic-ingress\].*' vd.pull.log | tail -1 | cut -c16-70)"
      grep -A1 '^[0-9:.]* IP ' cap.$port.txt | paste - - - 2>/dev/null | sed -n '1500,1524p' | sed -E 's/^([0-9:.]+) IP \(tos ([^,]*), ttl ([0-9]+), id ([0-9]+), offset ([0-9]+), flags \[([^]]*)\], proto UDP \(17\), length ([0-9]+)\).*UDP, length ([0-9]+).*/\1 id=\4 flags=[\6] iplen=\7 udplen=\8/' | sed -E 's/\[(bad udp cksum[^]]*)\]/CK:\1/' | cut -c1-110
}
cap "sender SEND_ZC + GSO" "X=1"
cap "sender sendmsg + GSO" "EMBR_QUIC_EGRESS=sendmsg"
