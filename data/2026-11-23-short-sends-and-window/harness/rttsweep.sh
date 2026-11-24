#!/bin/bash
# rttsweep.sh <runs> <delay each way>... : the delayed-path sweep before and after the change.
# before = the tree at 2c2b26d (ACKs through SEND_ZC, 4 MiB flow-control window), after = the same tree
# with short sends copied and a 16 MiB window. Both ends at their defaults except the push, which
# uses sendmsg + GSO: netem would hold a SEND_ZC sender's slots for the path delay, which a NIC does not.
set -uo pipefail
S=${SCRATCH:?scratch directory}
BEFORE=${BEFORE:-$S/async7/build/embr}; AFTER=${AFTER:-$S/async8/build/embr}
runs=$1; shift
echo "# delayed-path sweep, before and after; veth A-R-B, rate limit 2200mbit, limit 3000 packets, 1 GiB, $runs runs per delay, receiver CPUs 2,3; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
for delay in "$@"; do
    SCRATCH=$S unshare --user --map-root-user --net --mount bash "$S/async4/vethsb.sh" 2200mbit "$delay" 3000 "$S/realperf/f1g.bin" "$runs" \
        "before=$BEFORE" "after=$AFTER" 2>&1 | sed "s|$S|\$SCRATCH|g"
    echo "# last pull of this delay: $(grep -a '^\[quic-egress\]' "$S/async4/veth/pull.log" | tail -1)"
done
