#!/bin/bash
# receive-path prototype: loopback measurements into the repo's data/, then correctness checks
S=$SCRATCH
D=$REPO/data/2026-11-17-receive-path
OLD=$S/build-126-Release; NEW=$S/rx-tree/build-Release
DEPS=$REPO/cmake-build-release/_deps
mkdir -p "$D"
cd $REPO || exit 1

variant() { # variant <name> <build> <rows> [ENV=VAL ...]
    local name=$1 build=$2 rows=$3; shift 3
    env "$@" HOME=$S/home126 BUILD=$build CACHE_DISCIPLINE=keep FILE=$S/realperf/f1g.bin OUT=$S/realperf/out1117.bin \
        RESULTS=$D/$name.txt ROWS="$rows" RUNS=5 WARMUP=1 bash bench/bench.sh loopback > "$D/$name.terminal.txt" 2>&1
    echo "$name rc=$? $(grep -A4 '^row  ' "$D/$name.terminal.txt" | grep '^embr' | awk '{print $1"="$3}' | tr '\n' ' ')"
}
G="embrquicgso embrquicsmgso"
variant a-current-tree            $OLD "$G"
variant b-read-before-wait        $NEW "$G" EMBR_QUIC_GRO=0 EMBR_QUIC_RECV_BURST=1
variant c-burst64-no-gro          $NEW "$G" EMBR_QUIC_GRO=0
variant d-gro-burst1              $NEW "$G" EMBR_QUIC_RECV_BURST=1
variant e-gro-burst64             $NEW "$G"
variant f-gro-burst16             $NEW "$G" EMBR_QUIC_RECV_BURST=16
variant g-gro-burst256            $NEW "$G" EMBR_QUIC_RECV_BURST=256
variant h-gro-burst64-ack10       $NEW "$G" EMBR_QUIC_ACK_THRESH=10
variant i-gro-burst64-sender-no-gso $NEW "embrquic embrquicsm"
variant j-current-tree-again      $OLD "$G"
variant k-tcp-reference           $NEW "embrtcp"
: > "$S/realperf/out1117.bin"
echo BENCH_DONE

# receiver profile, before and after
cd $S/rxperf
./one.sh prop1 $NEW/embr 15111 > prop1.run.txt 2>&1
python3 rxcat.py current=base2.data prototype=prop1.data > "$D/receiver-profile.txt" 2>&1
cat base2.push.log | grep -ao '\[quic-egress\].*' | tail -1 | sed 's/^/# current sender:   /' >> "$D/receiver-profile.txt"
cat prop1.run.txt | sed 's/^/# prototype: /' >> "$D/receiver-profile.txt"
echo PROFILE_DONE

# correctness: Debug and sanitizer builds of the prototype, load gate, loss
export HOME=$S/home126
cd $S/rx-tree
cmake -S . -B build-Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_SOURCE_DIR_HTTPLIB=$DEPS/httplib-src -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$DEPS/googletest-src > cfg-Debug.log 2>&1 \
    && cmake --build build-Debug > build-Debug.log 2>&1; echo "debug build rc=$? warnings=$(grep -c 'warning' build-Debug.log)"
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
    -DFETCHCONTENT_SOURCE_DIR_HTTPLIB=$DEPS/httplib-src -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$DEPS/googletest-src > cfg-asan.log 2>&1 \
    && cmake --build build-asan > build-asan.log 2>&1; echo "asan build rc=$? warnings=$(grep -c 'warning' build-asan.log)"
./build-Debug/embr_test > unit-Debug.log 2>&1; echo "unit rc=$? $(grep -E '^\[  (FAILED|PASSED) ' unit-Debug.log | head -3 | tr '\n' ' ')"
bash $S/checks/gate4.sh $S/rx-tree/build-Debug/embr_test rx
O=$S/checks/gate-rx-nogro; mkdir -p $O
for r in $(seq 1 12); do for j in $(seq 1 8); do
    EMBR_QUIC_GRO=0 ./build-Debug/embr_test --gtest_filter='QuicTransport.*' > $O/g_${r}_${j}.log 2>/dev/null &
done; wait; done
echo "gate rx GRO off: failed runs $(grep -l 'FAILED' $O/g_*.log 2>/dev/null | wc -l)/96, passed runs $(grep -l 'PASSED' $O/g_*.log | wc -l)/96"
grep -h "^\[  FAILED  \] QuicTransport" $O/g_*.log 2>/dev/null | sort | uniq -c | head -5
echo "== 10% loss, 24 per mode"; bash $S/checks/loss4.sh $S/rx-tree/build-Release/embr rx 10 24
echo "== 10% loss, 24 per mode, GRO off"; EMBR_QUIC_GRO=0 bash $S/checks/loss4.sh $S/rx-tree/build-Release/embr rx-nogro 10 24
echo "== ASan/UBSan unit suite"; timeout 900 ./build-asan/embr_test > unit-asan.log 2>&1; echo "asan unit rc=$? $(grep -E '^\[  (FAILED|PASSED) ' unit-asan.log | head -3 | tr '\n' ' ') sanitizer reports: $(grep -c 'ERROR: AddressSanitizer\|runtime error' unit-asan.log)"
echo ALL_DONE
