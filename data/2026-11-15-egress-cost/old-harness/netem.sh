#!/bin/bash
# dummy device behind a 100 us netem delay: packets (and SEND_ZC completions) are released later, from a timer
cd "$(dirname "$0")"
ip link set lo up; ip link add dummy0 type dummy; ip addr add 10.99.0.1/24 dev dummy0; ip link set dummy0 up
tc qdisc add dev dummy0 root netem delay ${DELAY:-100us} limit 400000 || { echo "netem unavailable"; exit 1; }
tc qdisc show dev dummy0
for cfg in "2 0 600000" "2 1 1200000" "16 1 4000000"; do
  set -- $cfg
  for mode in sendmsg zc "zc 3200"; do
    for r in 1 2 3; do
      taskset -c 4 ./egress_bench ${mode%% *} 10.99.0.2 19001 1200 $1 $2 $3 $( [ "$mode" = "zc 3200" ] && echo 3200 )
    done | awk -v m="$mode" -v b=$1 -v g=$2 '{for(i=1;i<=NF;i++){split($i,a,"="); v[a[1]]=a[2]} cpu=(v["user"]+v["sys"]+v["softirq"])*1e9/v["sends"]; w=v["wall"]*1e9/v["sends"]; s=s" "sprintf("%.0f",cpu); ww=ww" "sprintf("%.0f",w); c=v["copied"]; e=v["errs"]} END{printf "burst=%s gso=%s %-8s cpu_ns/send:%s   wall_ns/send:%s  copied=%s errs=%s\n", b,g,m,s,ww,c,e}'
  done
done
