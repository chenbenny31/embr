#!/bin/bash
# defsweep.sh <runs> <delay each way>... : both ends at their defaults (no EMBR_* switch) on the delayed path,
# the tree at 2c2b26d against the tree with the egress choosing for itself and 48 packets per send.
set -uo pipefail
S=${SCRATCH:?scratch directory}
BEFORE=${BEFORE:-$S/async7/build/embr}; AFTER=${AFTER:-$S/async9/build/embr}
runs=$1; shift
echo "# delayed path, both ends at defaults; veth A-R-B, rate limit 2200mbit, limit 3000 packets, 1 GiB, $runs runs per delay, receiver CPUs 2,3; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
for delay in "$@"; do
    SCRATCH=$S unshare --user --map-root-user --net --mount bash "$S/async9/vethdef.sh" 2200mbit "$delay" 3000 "$S/realperf/f1g.bin" "$runs" \
        "before=$BEFORE" "after=$AFTER" 2>&1 | sed "s|$S|\$SCRATCH|g"
done
