#!/bin/bash
# checks on the resume-path change: sanitizer build and tests, then the pull and hash tests under load
set -uo pipefail
S=${SCRATCH:?scratch directory}; REPO=${REPO:?embr checkout whose build/_deps holds httplib and googletest}; cd "$S/resume" || exit 1
echo "# checks on the resume-path change; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
HOME=$S/home126 cmake -S tree -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
    -DFETCHCONTENT_SOURCE_DIR_HTTPLIB=$REPO/build/_deps/httplib-src -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$REPO/build/_deps/googletest-src > cmake-asan.log 2>&1 \
    && HOME=$S/home126 cmake --build build-asan > build-asan.log 2>&1
echo "asan build exit $?"
for m in "" "EMBR_HASH_LANES=1"; do
    HOME=$S/home126 env $m ./build-asan/embr_test > test-asan.log 2>&1
    echo "asan [${m:-default}] exit $? $(grep -E '^\[  PASSED  \]|^\[  FAILED  \]' test-asan.log | tr '\n' ' ') reports=$(grep -c 'ERROR: AddressSanitizer\|runtime error' test-asan.log)"
done
for m in "" "EMBR_HASH_LANES=1"; do
    FILTER='RunPull.*:ParallelHash.*:PartialFile.*' SCRATCH=$S bash gate.sh "$S/resume/build-tree/embr_test" 12 8 $m
done
SCRATCH=$S bash gate.sh "$S/resume/build-tree/embr_test" 12 8
echo "# done $(date -u +%Y-%m-%dT%H:%M:%SZ)"
