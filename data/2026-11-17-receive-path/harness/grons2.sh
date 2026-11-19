#!/bin/bash
# routed A-R-B: does GRO at b0 coalesce forwarded packets? python tool and embr
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
cd $S/rxperf; port=21000
py() { port=$((port+1)); $B python3 grotest.py recv $port $2 > gro.r.txt & sleep 0.3; python3 grotest.py send 10.2.0.2 $port 1 1 1; wait; echo "python, $1: $(cat gro.r.txt)"; }
em() { port=$((port+1)); EMBR_QUIC_STATS=1 timeout 60 $E push $S/realperf/f64.bin --port $port --transport quic --cert $CERT --key $KEY > vd.push.log 2>&1 & sleep 0.5
      EMBR_QUIC_STATS=1 timeout 60 $B $E pull 10.1.0.1 --port $port --transport quic --out $S/rxperf/vd.out.bin > vd.pull.log 2>&1; wait; : > vd.out.bin
      echo "embr, $1: sender $(grep -ao 'datagrams=[0-9]* sends=[0-9]*' vd.push.log | head -1) | receiver $(grep -ao '\[quic-ingress\].*' vd.pull.log | tail -1)"; }
ethtool -K a0 tx-udp-segmentation off tso off >/dev/null; $B ethtool -K b0 gro on >/dev/null; $R ethtool -K r1 tso off >/dev/null
py "routed, a0 no USO, r1 tso off, b0 gro on" 10.1.0.1
em "routed, same"
$R ethtool -K r0 gro on >/dev/null; ethtool -K a0 tso off >/dev/null
py "+ r0 gro on (NAPI at the router's ingress)" 10.1.0.1
em "+ r0 gro on"
$R ethtool -K r0 rx-udp-gro-forwarding on >/dev/null
py "+ r0 rx-udp-gro-forwarding on" 10.1.0.1
em "+ r0 rx-udp-gro-forwarding on"
