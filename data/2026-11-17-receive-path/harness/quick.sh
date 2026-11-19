#!/bin/bash
# quick.sh <tag> <embr> <port> [ENV=VAL ...]: one unpinned 1 GiB loopback transfer, receiver timed, stats from both ends
S=$SCRATCH
D=$S/rxperf; tag=$1; EMBR=$2; port=$3; shift 3
export HOME=$S/home126
CERT=$HOME/.config/embr/cert.pem; KEY=$HOME/.config/embr/key.pem
cd $D
env "$@" EMBR_QUIC_STATS=1 /usr/bin/time -f "send_user=%U send_sys=%S" -o $tag.stime $EMBR push $S/realperf/f1g.bin --port $port --transport quic --cert $CERT --key $KEY > $tag.push.log 2>&1 &
sp=$!
sleep 0.6
env "$@" EMBR_QUIC_STATS=1 /usr/bin/time -f "wall=%e recv_user=%U recv_sys=%S" -o $tag.rtime $EMBR pull 127.0.0.1 --port $port --transport quic --out out.q.bin > $tag.pull.log 2>&1; rc=$?
wait $sp
ok=MISMATCH; cmp -s $S/realperf/f1g.bin out.q.bin && ok=intact; : > out.q.bin
w=$(sed -n 's/.*wall=\([0-9.]*\).*/\1/p' $tag.rtime)
echo "$tag rc=$rc $ok gbps=$(awk -v w=$w 'BEGIN{printf "%.2f", 8*1073741824/w/1e9}') $(cat $tag.rtime) $(cat $tag.stime)"
echo "   sender:   $(grep -ao '\[quic-egress\].*' $tag.push.log | tail -1 | cut -c1-110)"
echo "   sender:   $(grep -ao '\[quic-ingress\].*' $tag.push.log | tail -1)"
echo "   receiver: $(grep -ao '\[quic-ingress\].*' $tag.pull.log | tail -1)"
echo "   receiver: $(grep -ao '\[quic-egress\].*' $tag.pull.log | tail -1 | cut -c1-90)"
