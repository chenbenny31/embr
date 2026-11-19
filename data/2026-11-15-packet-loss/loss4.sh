#!/usr/bin/env bash
# loss4.sh <embr binary> <label> <loss%> <n per mode> [ack_thresh]: 20 MB QUIC transfers under loss
# in each of the four egress x GSO modes, 8 at a time
E=$1; L=$2; LOSS=$3; N=$4; ACK=${5:-}
S=$SCRATCH
W=$S/checks/loss4-$L; rm -rf $W; mkdir -p $W; cd $W; head -c 20000000 /dev/urandom > f.bin; export HOME=$S/home126
[ -n "$ACK" ] && export EMBR_QUIC_ACK_THRESH=$ACK
unshare --user --map-root-user --net bash -c "
  ip link set lo up
  nft add table inet t && nft add chain inet t in '{ type filter hook input priority 0; }' && nft add rule inet t in udp dport 15000-15999 numgen random mod 100 \\< $LOSS drop && nft add rule inet t in udp sport 15000-15999 numgen random mod 100 \\< $LOSS drop || exit 7
  one() { ( env \$ENVS $E push f.bin --transport quic --port \$1 > \$1.push.out 2> \$1.push.log ) & sleep 0.5; s=\$(date +%s.%N)
    env \$ENVS timeout 300 $E pull 127.0.0.1 --port \$1 --transport quic --out \$1.bin > \$1.pull.out 2> \$1.pull.log; rc=\$?; e=\$(date +%s.%N); wait
    if [ \$rc = 0 ] && cmp -s f.bin \$1.bin; then echo \"ok \$(echo \"\$e - \$s\" | bc)\" >> results.\$MODE; rm -f \$1.*; else echo \"FAIL \$1 rc=\$rc \$(echo \"\$e - \$s\" | bc) \$(grep -a '\\[error\\]' \$1.pull.log \$1.push.log | cut -c1-150 | tr '\n' ' ')\" >> results.\$MODE; rm -f \$1.bin; fi; }
  base=15000
  for MODE in zc-gso zc-nogso sendmsg-gso sendmsg-nogso; do
    ENVS='EMBR_X=1'; case \$MODE in sendmsg-*) ENVS=\"\$ENVS EMBR_QUIC_EGRESS=sendmsg\";; esac; case \$MODE in *-nogso) ENVS=\"\$ENVS EMBR_QUIC_GSO=0\";; esac
    : > results.\$MODE
    for batch in \$(seq 1 \$(( $N / 8 ))); do for k in 1 2 3 4 5 6 7 8; do base=\$((base+1)); one \$base & done; wait; done
    echo \"loss $LOSS% ${ACK:+ack_thresh=$ACK }\$MODE: \$(grep -c '^ok' results.\$MODE) ok of \$(wc -l < results.\$MODE); seconds min/median/max: \$(grep '^ok' results.\$MODE | awk '{print \$2}' | sort -n | awk '{a[NR]=\$1} END{printf \"%.1f / %.1f / %.1f\", a[1], a[int((NR+1)/2)], a[NR]}')\"
    grep FAIL results.\$MODE | cut -c1-260 | head -4
  done" 2>&1
echo LOSS4_DONE
