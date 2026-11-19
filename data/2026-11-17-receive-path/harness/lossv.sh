#!/bin/bash
# lossv.sh <loss%> <n> <label>=<embr>[,ENV=VAL...] ...   20 MB QUIC transfers under random loss both ways, 8 at a time, default egress
S=$SCRATCH
LOSS=$1; N=$2; shift 2
W=$S/rxperf/lossv; mkdir -p $W; cd $W; [ -f f.bin ] || head -c 20000000 /dev/urandom > f.bin; export HOME=$S/home126
ip link set lo up
nft add table inet t && nft add chain inet t in '{ type filter hook input priority 0; }' && nft add rule inet t in udp dport 15000-15999 numgen random mod 100 \< $LOSS drop && nft add rule inet t in udp sport 15000-15999 numgen random mod 100 \< $LOSS drop || exit 7
one() { # one <port> <label> <embr> <envs>
  ( env $4 EMBR_QUIC_STATS=1 $3 push f.bin --transport quic --port $1 > $1.push.out 2> $1.push.log ) & sleep 0.5; s=$(date +%s.%N)
  env $4 EMBR_QUIC_STATS=1 timeout 300 $3 pull 127.0.0.1 --port $1 --transport quic --out $1.bin > $1.pull.out 2> $1.pull.log; rc=$?; e=$(date +%s.%N); wait
  if [ $rc = 0 ] && cmp -s f.bin $1.bin; then echo "ok $(echo "$e - $s" | bc) $(grep -ao 'datagrams=[0-9]* sends=[0-9]*' $1.push.log | head -1)" >> results.$2; else echo "FAIL $1 rc=$rc" >> results.$2; fi; : > $1.bin; }
base=15000
for spec in "$@"; do
  label=${spec%%=*}; rest=${spec#*=}; E=${rest%%,*}; envs=$(echo "${rest#$E}" | tr ',' ' '); [ -z "$envs" ] && envs="EMBR_X=1"
  : > results.$label
  for batch in $(seq 1 $(( N / 8 ))); do for k in 1 2 3 4 5 6 7 8; do base=$((base+1)); one $base $label $E "$envs" & done; wait; done
  echo "loss $LOSS% $label: $(grep -c '^ok' results.$label) ok of $(wc -l < results.$label); seconds min/median/max: $(grep '^ok' results.$label | awk '{print $2}' | sort -n | awk '{a[NR]=$1} END{printf "%.1f / %.1f / %.1f", a[1], a[int((NR+1)/2)], a[NR]}'); sender datagrams median: $(grep '^ok' results.$label | sed 's/.*datagrams=\([0-9]*\).*/\1/' | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')"
done
