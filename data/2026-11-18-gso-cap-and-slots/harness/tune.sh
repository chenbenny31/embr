#!/bin/bash
# packets per send (GSO cap) x read burst x SEND_ZC slots: what the 8 MiB locked-memory default allows and what it buys
S=$SCRATCH
D=$REPO/data/2026-11-18-gso-cap-and-slots
T=$S/tune-tree/build-Release
mkdir -p "$D"
cd $REPO || exit 1

# E5: how many slots one connection can register under ulimit -l (dummy device, private namespace)
cat > $S/rxperf/memlock_ns.sh <<'EOF'
#!/bin/bash
BIN=$1
ip link set lo up; ip link add dummy0 type dummy; ip addr add 10.99.0.1/24 dev dummy0; ip link set dummy0 up
echo "# ulimit -l = $(ulimit -l) KiB, kernel $(uname -r); slab = slots x packets x 1350 bytes; one process"
for segs in 16 32 48; do
  for slots in 8 16 32 64 96 128 160 192 256 320 384 388 400 512; do
    kib=$(( slots * segs * 1350 / 1024 ))
    if $BIN zc 10.99.0.2 1350 $segs $segs $(( segs * 200 )) $slots > /dev/null 2> /tmp/.ml.err; then r=registered; else r="refused ($(head -c 60 /tmp/.ml.err | tr '\n' ' '))"; fi
    echo "packets=$segs slots=$slots slab=${kib}KiB $r"
  done
done
EOF
c++ -std=c++20 -O2 -DNDEBUG -Isrc bench/egress_cost.cpp src/transport/quic_egress.cpp -luring -o $S/rxperf/egress_cost_tune
unshare --user --map-root-user --net bash $S/rxperf/memlock_ns.sh $S/rxperf/egress_cost_tune > "$D/memlock-budget.txt" 2>&1
echo "E5 done: $(grep -c registered "$D/memlock-budget.txt") registered, $(grep -c refused "$D/memlock-budget.txt") refused"

# E1: loopback transfers, 1 GiB, n = 5, both egress paths
for cfg in "16 64" "16 256" "24 64" "24 128" "32 64" "32 128" "32 256" "48 64" "48 128" "48 256"; do
    set -- $cfg; name=loopback-packets$1-burst$2
    EMBR_QUIC_GSO_SEGS=$1 EMBR_QUIC_RECV_BURST=$2 HOME=$S/home126 BUILD=$T CACHE_DISCIPLINE=keep FILE=$S/realperf/f1g.bin OUT=$S/realperf/out1117.bin \
        RESULTS=$D/$name.txt ROWS="embrquicgso embrquicsmgso" RUNS=5 WARMUP=1 bash bench/bench.sh loopback > "$D/$name.terminal.txt" 2>&1
    echo "$name rc=$? $(grep -A3 '^row  ' "$D/$name.terminal.txt" | grep '^embr' | awk '{print $1"="$3}' | tr '\n' ' ') $(grep -o '([0-9.]* datagrams per send' "$D/$name.terminal.txt" | tr '\n' ' ')"
done
: > "$S/realperf/out1117.bin"
echo E1_DONE

# E4: send-only cost with packets held 100 us, by slot count
for slots in 4 8 16 32 64; do
    BIN=$S/rxperf/egress_cost_tune DELAY=100us SLOTS=$slots SEGS_LIST="16 24 32 47" REPS=3 bash bench/egress_cost.sh > "$D/egress-cost-held-100us-${slots}slots.txt" 2>&1
    echo "E4 slots=$slots rc=$?"
done
BIN=$S/rxperf/egress_cost_tune SEGS_LIST="16 24 32 40 47 48" REPS=5 bash bench/egress_cost.sh > "$D/egress-cost-freed-at-once.txt" 2>&1
echo E4_DONE

# E2/E3: veth, packets one by one, sendmsg sender
cd $S/rxperf
SM=EMBR_QUIC_EGRESS=sendmsg; E=$T/embr
A="packets16-burst64=$E,$SM,EMBR_QUIC_GSO_SEGS=16,EMBR_QUIC_RECV_BURST=64"
B="packets32-burst128=$E,$SM,EMBR_QUIC_GSO_SEGS=32,EMBR_QUIC_RECV_BURST=128"
C="packets48-burst256=$E,$SM,EMBR_QUIC_GSO_SEGS=48,EMBR_QUIC_RECV_BURST=256"
unshare --user --map-root-user --net --mount bash veth.sh none    none none 1000  $S/realperf/f1g.bin 3 "$A" "$B" "$C" > "$D/veth-unshaped.txt" 2>&1; echo "veth unshaped: $(grep -c ' intact ' $D/veth-unshaped.txt)"
unshare --user --map-root-user --net --mount bash veth.sh 100mbit 10ms none 100   $S/realperf/f64.bin 3 "$A" "$B" "$C" > "$D/veth-100mbit-rtt20ms-q100.txt" 2>&1; echo "veth 100mbit: $(grep -c ' intact ' $D/veth-100mbit-rtt20ms-q100.txt)"
unshare --user --map-root-user --net --mount bash veth.sh none    10ms 1    10000 $S/rxperf/f20.bin    3 "$A" "$C" > "$D/veth-rtt20ms-loss1.txt" 2>&1; echo "veth loss1: $(grep -c ' intact ' $D/veth-rtt20ms-loss1.txt)"
grep -rlI -e "$S" -e '$HOME' "$D" | while read -r f; do sed -i -e "s|$S|\$SCRATCH|g" -e 's|$REPO|$REPO|g' -e 's|$HOME|$HOME|g' "$f"; done
echo TUNE_DONE
