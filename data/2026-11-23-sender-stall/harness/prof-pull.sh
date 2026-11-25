#!/bin/bash
# prof-pull.sh <label> <perf event> [pull ENV=value ...]: the receiver's profile of one loopback 1 GiB QUIC transfer
set -uo pipefail
S=${SCRATCH:?scratch directory holding the build, a 1 GiB file and a home with the QUIC cert}
embr=${EMBR:-$S/cipher/build/embr}
file=${FILE:-$S/realperf/f1g.bin}
work=${WORK:-$S/cipher}
out=$work/out.bin
label=$1 event=$2; shift 2
port=10034
( HOME=$S/home126 taskset -c 6-9 "$embr" push "$file" --port $port --transport quic --cert "$S/home126/.config/embr/cert.pem" --key "$S/home126/.config/embr/key.pem" > /dev/null 2>&1 & )
until [[ -n "$(ss -H -lnu "sport = :$port" 2>/dev/null)" ]]; do sleep 0.05; done
rm -f "$out" "$work/.out.bin.embr.partial"
HOME=$S/home126 taskset -c 2-5 env "$@" perf record -q -e "$event" -F 4999 -o "$work/$label.data" \
    "$embr" pull 127.0.0.1 --port $port --transport quic --out "$out" > /dev/null 2>&1
waited=0
while pgrep -f "[e]mbr push .*--port $port" > /dev/null; do sleep 0.05; waited=$((waited + 1)); (( waited == 100 )) && pkill -f "[e]mbr push .*--port $port"; done
rm -f "$out"
echo "== $label ($event): self time by thread and symbol, percent of the pull process's samples"
perf report -i "$work/$label.data" --no-children --sort comm,symbol --stdio 2>/dev/null | grep -E '^ +[0-9.]+%' | head -${TOP:-26}
