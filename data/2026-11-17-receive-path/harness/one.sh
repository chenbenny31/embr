#!/bin/bash
# one 1 GiB loopback QUIC transfer with the RECEIVER under perf: one.sh <tag> <embr> <port> [ENV=VAL ...]
S=$SCRATCH
D=$S/rxperf; tag=$1; EMBR=$2; port=$3; shift 3
export HOME=$S/home126
CERT=$HOME/.config/embr/cert.pem; KEY=$HOME/.config/embr/key.pem
cd $D
env "$@" EMBR_QUIC_STATS=1 taskset -c 4 $EMBR push $S/realperf/f1g.bin --port $port --transport quic --cert $CERT --key $KEY > $tag.push.log 2>&1 &
sp=$!
sleep 0.7
t0=$(date +%s.%N)
env "$@" EMBR_QUIC_STATS=1 perf record -q -g -e task-clock -c 50000 -o $D/$tag.data -- taskset -c 6 \
    $EMBR pull 127.0.0.1 --port $port --transport quic --out out.$tag.bin > $tag.pull.log 2>&1; rc=$?
t1=$(date +%s.%N)
wait $sp
echo "$tag rc=$rc secs=$(echo "$t1 - $t0" | bc) $(grep -ao "\[quic-egress\].*" $tag.push.log | tail -1)"
grep -ao "\[quic-[a-z]*\].*" $tag.pull.log | tail -2
if cmp -s $S/realperf/f1g.bin out.$tag.bin; then echo "$tag intact"; else echo "$tag MISMATCH"; fi; : > out.$tag.bin
