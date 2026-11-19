#!/bin/bash
# NIC-like veth runs for the receive-path prototype; raw output into the repo's data/
# the sender uses sendmsg: io_uring zero-copy packets cross veth uncopied and GRO will not merge them,
# which a wire cannot do
S=$SCRATCH
D=$REPO/data/2026-11-17-receive-path
O=$S/build-126-Release/embr; N=$S/rx-tree/build-Release/embr
SM=EMBR_QUIC_EGRESS=sendmsg
cd $S/rxperf
v() { # v <outfile> <rate> <delay> <loss> <limit> <file> <runs> specs...
    local out=$1; shift
    unshare --user --map-root-user --net --mount bash veth.sh "$@" > "$D/$out" 2>&1
    echo "$out: $(grep -c ' intact ' "$D/$out") intact of $(grep -c '^run' "$D/$out")"
}
C="current=$O,$SM"; P="prototype=$N,$SM"; PN="prototype-gro-off=$N,$SM,EMBR_QUIC_GRO=0"; P16="prototype-burst16=$N,$SM,EMBR_QUIC_RECV_BURST=16"
v veth-unshaped.txt              none    none none 1000 $S/realperf/f1g.bin 3 "$C" "$P" "$PN"
v veth-1gbit-rtt10ms.txt         1gbit   5ms  none 1000 $S/realperf/f1g.bin 2 "$C" "$P" "$PN"
v veth-100mbit-rtt20ms-q100.txt  100mbit 10ms none 100  $S/realperf/f64.bin 3 "$C" "$P" "$P16"
v veth-rtt20ms-loss0.1.txt       none    10ms 0.1  10000 $S/realperf/f64.bin 3 "$C" "$P" "$P16"
v veth-rtt20ms-loss1.txt         none    10ms 1    10000 $S/rxperf/f20.bin  3 "$C" "$P" "$P16"
v veth-rtt20ms-loss5.txt         none    10ms 5    10000 $S/rxperf/f5.bin   3 "$C" "$P" "$P16"
v veth-rtt20ms-loss10.txt        none    10ms 10   10000 $S/rxperf/f5.bin   3 "$C" "$P" "$P16"
echo VETH_DONE
