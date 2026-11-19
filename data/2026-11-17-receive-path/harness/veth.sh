#!/bin/bash
# sender A -- router R -- receiver B over veth, set up so packets travel one by one as on a wire:
# A's device has no UDP segmentation offload, so the kernel cuts every GSO batch into packets
# before it leaves; R forwards single packets, optionally through a rate-limited queue; B's
# device runs NAPI + GRO as a NIC does. Loopback instead hands a GSO batch over whole.
#
#   unshare --user --map-root-user --net --mount bash veth.sh <rate|none> <delay|none> <loss%|none> <limit> <file> <runs> <label>=<embr binary>[,ENV=VAL...] ...
S=$SCRATCH
RATE=$1; DELAY=$2; LOSS=$3; LIMIT=$4; FILE=$5; RUNS=$6; shift 6
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
# packets one by one on the wire, GRO at the receiving device
ethtool -K a0 tx-udp-segmentation off >/dev/null 2>&1 || echo "could not switch UDP segmentation offload off on a0"
$B ethtool -K b0 gro on >/dev/null 2>&1 || echo "could not switch GRO on on b0"
# veth runs NAPI + GRO on b0 only for packets whose sending device (r1) has TSO off
$R ethtool -K r1 tso off >/dev/null 2>&1 || echo "could not switch TSO off on r1"
if [ "$RATE" != none ] || [ "$DELAY" != none ] || [ "$LOSS" != none ]; then
    spec=""; [ "$DELAY" != none ] && spec="delay $DELAY"; [ "$LOSS" != none ] && spec="$spec loss $LOSS%"; back=$spec
    [ "$RATE" != none ] && spec="$spec rate $RATE"
    $R tc qdisc add dev r1 root netem $spec limit $LIMIT || { echo "netem failed"; exit 1; }      # data direction
    [ -n "$back" ] && $R tc qdisc add dev r0 root netem $back limit 10000                          # ACK direction: same delay and loss
fi
$B ping -c1 -W2 10.1.0.1 >/dev/null || { echo "no route"; exit 1; }
echo "# veth A-R-B  rate=$RATE delay=$DELAY (each way) loss=$LOSS% (each way) limit=$LIMIT packets  file=$(stat -c%s $FILE) bytes  kernel=$(uname -r)"
echo "# a0: $(ethtool -k a0 | grep -E '^tx-udp-segmentation' | tr -s ' ')   b0: $($B ethtool -k b0 | grep -E '^generic-receive-offload' | tr -s ' ')"
bytes=$(stat -c%s $FILE); cd $S/rxperf; p=17000
for run in $(seq 1 $RUNS); do
  for spec in "$@"; do
    label=${spec%%=*}; rest=${spec#*=}; E=${rest%%,*}; envs=$(echo "${rest#$E}" | tr ',' ' ')
    p=$((p+1))
    env $envs EMBR_QUIC_STATS=1 timeout 300 $E push $FILE --port $p --transport quic --cert $CERT --key $KEY > vt.push.log 2>&1 &
    sleep 0.6
    t0=$(date +%s.%N); env $envs EMBR_QUIC_STATS=1 timeout 300 $B $E pull 10.1.0.1 --port $p --transport quic --out $S/rxperf/vt.out.bin > vt.pull.log 2>&1; rc=$?; t1=$(date +%s.%N)
    wait
    ok=MISMATCH; cmp -s $FILE vt.out.bin && ok=intact; : > vt.out.bin
    secs=$(echo "$t1 - $t0" | bc)
    echo "run$run $label rc=$rc $ok secs=$secs mbps=$(awk -v b=$bytes -v s=$secs 'BEGIN{printf "%.0f", b*8/s/1e6}') | sender $(grep -ao 'datagrams=[0-9]* sends=[0-9]*' vt.push.log | head -1) | receiver $(grep -ao '\[quic-ingress\].*' vt.pull.log | tail -1 | sed 's/\[quic-ingress\] //')"
  done
done
$R tc -s qdisc show dev r1 2>/dev/null | grep -E 'Sent|dropped' | head -2 | sed 's/^/# r1 qdisc: /'
