#!/bin/bash
# inside a private user+net namespace: lo and a dummy device that frees every packet at transmit
B=$(dirname "$0")/egress_bench
ip link set lo up
ip link add dummy0 type dummy && ip addr add 10.99.0.1/24 dev dummy0 && ip link set dummy0 up
N=${N:-2000000}; CPU=${CPU:-4}
for dst in ${DSTS:-127.0.0.1 10.99.0.2}; do
  for cfg in ${CFGS:-"2 0" "2 1" "10 0" "10 1" "16 1" "32 1"}; do
    for mode in ${MODES:-sendmsg usend zc}; do
      for pkt in ${PKTS:-1200}; do
        echo -n "dst=$dst "; taskset -c $CPU $B $mode $dst 19001 $pkt $cfg $N ${FLAGS:-}
      done
    done
  done
done
