#!/bin/bash
S=$SCRATCH
mount -t tmpfs tmpfs /run 2>/dev/null; mkdir -p /run/netns
ip netns add b || exit 1
ip link set lo up; ip link add a0 type veth peer name b0; ip link set b0 netns b
ip addr add 10.1.0.1/24 dev a0; ip link set a0 up
B="nsenter --net=/run/netns/b"; $B ip link set lo up; $B ip addr add 10.1.0.2/24 dev b0; $B ip link set b0 up
$B ping -c1 -W2 10.1.0.1 >/dev/null || exit 1
cd $S/rxperf; port=20000
t() { # t <desc> <recv connect ip|-> <df> <conn> <gso>
  port=$((port+1)); $B python3 grotest.py recv $port $2 > gro.r.txt & sleep 0.3; python3 grotest.py send 10.1.0.2 $port $3 $4 $5; wait; echo "$1: $(cat gro.r.txt)"; }
echo "--- direct veth A-B, defaults (GSO skbs pass whole)"
t "gso send, unconnected recv" - 0 1 1
ethtool -K a0 tx-udp-segmentation off tso off; $B ethtool -K b0 gro on
echo "--- a0: no UDP segmentation offload, tso off; b0: gro on (NAPI)"
t "gso send (software segments), unconnected recv, no DF" - 0 1 1
t "gso send (software segments), connected recv, no DF" 10.1.0.1 0 1 1
t "gso send (software segments), connected recv, DF probe" 10.1.0.1 1 1 1
t "16 single sends, connected recv, DF probe" 10.1.0.1 1 1 0
t "16 single sends, unconnected recv, no DF, unconnected send" - 0 0 0
$B sh -c 'echo 50000 > /sys/class/net/b0/gro_flush_timeout; echo 1 > /sys/class/net/b0/napi_defer_hard_irqs'
echo "--- b0: gro_flush_timeout 50us, napi_defer_hard_irqs 1"
t "gso send (software segments), connected recv, DF probe" 10.1.0.1 1 1 1
t "16 single sends, connected recv, DF probe" 10.1.0.1 1 1 0
