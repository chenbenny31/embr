#!/bin/bash
# checks.sh: unit tests in six modes, ASan+UBSan, and the QUIC load gate in four modes, on the scratch tree
set -uo pipefail
S=${SCRATCH:?scratch directory}; REPO=${REPO:?embr checkout whose build/_deps holds httplib and googletest}; cd "$S/async9" || exit 1
echo "# checks on the final scratch tree; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
for m in "" "EMBR_QUIC_EGRESS=zc" "EMBR_QUIC_EGRESS=sendmsg" "EMBR_QUIC_GSO_SEGS=16" "EMBR_QUIC_GSO=0" "EMBR_QUIC_GRO=0"; do
    HOME=$S/home126 env $m ./build/embr_test > test-mode.log 2>&1
    echo "unit [${m:-default}] exit $? $(grep -E '^\[  PASSED  \]|^\[  FAILED  \]|^\[  SKIPPED \] [0-9]' test-mode.log | tr '\n' ' ')"
done
HOME=$S/home126 cmake -S tree -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_CXX_FLAGS=$ASAN_FLAGS" "-DCMAKE_EXE_LINKER_FLAGS=$ASAN_LINK" \
    -DFETCHCONTENT_SOURCE_DIR_HTTPLIB=$REPO/build/_deps/httplib-src -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$REPO/build/_deps/googletest-src > cmake-asan.log 2>&1 \
    && HOME=$S/home126 cmake --build build-asan > build-asan.log 2>&1
echo "asan build exit $?"
for m in "" "EMBR_QUIC_EGRESS=zc" "EMBR_QUIC_GSO_SEGS=16"; do
    HOME=$S/home126 env $m ./build-asan/embr_test > test-asan.log 2>&1
    echo "asan [${m:-default}] exit $? $(grep -E '^\[  PASSED  \]|^\[  FAILED  \]' test-asan.log | tr '\n' ' ') reports=$(grep -c 'ERROR: AddressSanitizer\|runtime error' test-asan.log)"
done
for m in "" "EMBR_QUIC_EGRESS=zc" "EMBR_QUIC_EGRESS=sendmsg" "EMBR_QUIC_GSO_SEGS=16"; do
    SCRATCH=$S bash "$(dirname "$0")/gate.sh" "$S/async9/build/embr_test" 12 8 $m
done
echo "# done $(date -u +%Y-%m-%dT%H:%M:%SZ)"
