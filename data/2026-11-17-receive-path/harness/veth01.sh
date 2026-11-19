#!/bin/bash
S=$SCRATCH
D=$REPO/data/2026-11-17-receive-path
O=$S/build-126-Release/embr; N=$S/rx-tree/build-Release/embr; SM=EMBR_QUIC_EGRESS=sendmsg
cd $S/rxperf
unshare --user --map-root-user --net --mount bash veth.sh none 10ms 0.1 10000 $S/realperf/f64.bin 8 "current=$O,$SM" "prototype=$N,$SM" "prototype-ack-early4=$N,$SM,EMBR_QUIC_ACK_EARLY=4" > $D/veth-rtt20ms-loss0.1-n8.txt 2>&1
echo "loss0.1 n8: $(grep -c ' intact ' $D/veth-rtt20ms-loss0.1-n8.txt) intact of $(grep -c '^run' $D/veth-rtt20ms-loss0.1-n8.txt)"
echo V01_DONE
