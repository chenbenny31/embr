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
