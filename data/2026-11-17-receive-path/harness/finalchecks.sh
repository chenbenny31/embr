#!/bin/bash
# checks on the final receive-path tree (prototype knobs removed)
S=$SCRATCH
DEPS=$REPO/cmake-build-release/_deps
export HOME=$S/home126
cd $S/rx-final
bash $S/checks/gate4.sh $S/rx-final/build-Debug/embr_test rxf
O=$S/checks/gate-rxf-nogro; mkdir -p $O
for r in $(seq 1 12); do for j in $(seq 1 8); do
    EMBR_QUIC_GRO=0 ./build-Debug/embr_test --gtest_filter='QuicTransport.*' > $O/g_${r}_${j}.log 2>/dev/null &
done; wait; done
echo "gate rxf GRO off: failed runs $(grep -l 'FAILED' $O/g_*.log 2>/dev/null | wc -l)/96, passed runs $(grep -l 'PASSED' $O/g_*.log | wc -l)/96"
grep -h "^\[  FAILED  \] QuicTransport" $O/g_*.log 2>/dev/null | sort | uniq -c | head -5
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
    -DFETCHCONTENT_SOURCE_DIR_HTTPLIB=$DEPS/httplib-src -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$DEPS/googletest-src > cfg-asan.log 2>&1 \
    && cmake --build build-asan > build-asan.log 2>&1; echo "asan build rc=$? warnings=$(grep -c 'warning' build-asan.log)"
timeout 900 ./build-asan/embr_test > unit-asan.log 2>&1; echo "asan unit rc=$? $(grep -E '^\[  (FAILED|PASSED) ' unit-asan.log | head -3 | tr '\n' ' ') sanitizer reports: $(grep -c 'ERROR: AddressSanitizer\|runtime error' unit-asan.log)"
EMBR_QUIC_GRO=0 timeout 900 ./build-asan/embr_test --gtest_filter='QuicTransport.*' > unit-asan-nogro.log 2>&1; echo "asan unit GRO off rc=$? $(grep -E '^\[  (FAILED|PASSED) ' unit-asan-nogro.log | head -3 | tr '\n' ' ') sanitizer reports: $(grep -c 'ERROR: AddressSanitizer\|runtime error' unit-asan-nogro.log)"
echo FINAL_CHECKS_DONE
