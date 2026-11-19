#!/bin/bash
cd "$(dirname "$0")"
ip link set lo up; ip link add dummy0 type dummy; ip addr add 10.99.0.1/24 dev dummy0; ip link set dummy0 up
tc qdisc add dev dummy0 root netem delay ${DELAY:-100us} limit 400000 || exit 1
row() { # label burst gso n env...
  local label=$1 b=$2 g=$3 n=$4; shift 4
  for r in 1 2 3; do sleep 0.3; env "$@" taskset -c 4 ./egress_bench $MODE 10.99.0.2 19001 1200 $b $g $n $FLAGS; done |
  awk -v l="$label" '{for(i=1;i<=NF;i++){split($i,a,"="); v[a[1]]=a[2]} s=s" "sprintf("%.0f",(v["user"]+v["sys"]+v["softirq"])*1e9/v["sends"]); ww=ww" "sprintf("%.0f",v["wall"]*1e9/v["sends"])} END{printf "%-34s cpu_ns/send:%s   wall_ns/send:%s\n", l,s,ww}'
}
for cfg in "2 1 1200000" "16 1 4000000"; do
  set -- $cfg
  MODE=sendmsg FLAGS= row "burst=$1 sendmsg" $1 $2 $3 X=1
  MODE=zc FLAGS= row "burst=$1 zc 8 slots (embr)" $1 $2 $3 X=1
  MODE=zc FLAGS= row "burst=$1 zc 128 slots" $1 $2 $3 SLOTS=128
  MODE=zc FLAGS=3200 row "burst=$1 zc 128 slots, DEFER_TASKRUN" $1 $2 $3 SLOTS=128
done
