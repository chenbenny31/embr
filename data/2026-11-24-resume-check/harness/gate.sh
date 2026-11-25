#!/bin/bash
# gate.sh <test binary> <rounds> <concurrent> [ENV=value ...]: the QUIC load gate.
# Each round starts <concurrent> runs of the QuicTransport suite at once; prints failures and a total.
set -uo pipefail
S=${SCRATCH:?scratch directory}
bin=$1 rounds=$2 conc=$3; shift 3
dir=$(mktemp -d "$S/resume/gate.XXXXXX")
failed=0 total=0
for (( r = 1; r <= rounds; r++ )); do
    pids=()
    for (( c = 1; c <= conc; c++ )); do
        HOME=$S/home126 env "$@" "$bin" --gtest_filter="${FILTER:-QuicTransport.*:QuicSocket.*}" > "$dir/r$r.c$c.log" 2>&1 &
        pids+=($!)
    done
    c=0
    for pid in "${pids[@]}"; do
        c=$((c + 1)); total=$((total + 1))
        if ! wait "$pid"; then
            failed=$((failed + 1))
            echo "round $r run $c FAILED: $(grep -E '^\[  FAILED  \]|Failure|terminate|Sanitizer' "$dir/r$r.c$c.log" | head -4 | tr '\n' ' ')"
        fi
    done
done
echo "gate [${*:-default}]: $total runs, $failed failed"
(( failed == 0 )) && rm -rf "$dir"
exit $(( failed > 0 ))
