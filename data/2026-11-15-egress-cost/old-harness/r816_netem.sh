#!/bin/bash
# 8-16 packets per send with completions delayed 100 us (netem on the dummy device), 1350-byte packets
cd "$(dirname "$0")"
ip link set lo up; ip link add dummy0 type dummy; ip addr add 10.99.0.1/24 dev dummy0; ip link set dummy0 up
tc qdisc add dev dummy0 root netem delay ${DELAY:-100us} limit 400000 || exit 1
row() { # label burst n env...
  local label=$1 b=$2 n=$3; shift 3
  for r in 1 2 3; do sleep 0.3; env "$@" taskset -c 4 ./egress_bench $MODE 10.99.0.2 19001 1350 $b 1 $n $FLAGS; done |
  awk -v l="$label" '{for(i=1;i<=NF;i++){split($i,a,"="); v[a[1]]=a[2]} c[NR]=(v["user"]+v["sys"]+v["softirq"])*1e9/v["sends"]; w[NR]=v["wall"]*1e9/v["sends"]; e+=v["errs"]; cp=v["copied"]}
     END{asort(c); asort(w); printf "%s\t%.0f\t%.0f\t%d\t%d\n", l, c[2], w[2], cp, e}'
}
echo -e "burst\tmode\tcpu_ns/send\twall_ns/send\tcopied\terrs"
for b in 8 10 12 14 16; do
  n=$(( b * 250000 ))
  MODE=sendmsg FLAGS= row "$b	sendmsg" $b $n X=1
  MODE=zc FLAGS= row "$b	zc slots=8" $b $n SLOTS=8
  MODE=zc FLAGS= row "$b	zc slots=16" $b $n SLOTS=16
  MODE=zc FLAGS= row "$b	zc slots=32" $b $n SLOTS=32
  MODE=zc FLAGS= row "$b	zc slots=128" $b $n SLOTS=128
  MODE=zc FLAGS=3200 row "$b	zc slots=128 defer" $b $n SLOTS=128
done
