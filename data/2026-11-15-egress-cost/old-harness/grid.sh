#!/bin/bash
cd "$(dirname "$0")"
for dev in dummy lo; do
  for cfg in "2 0" "2 1" "10 1" "16 1"; do
    python3 perfcat.py $dev 1200 $cfg 16
  done
done
python3 perfcat.py dummy 1350 16 1 16 sendmsg,zc
python3 perfcat.py dummy 1350 32 1 32 sendmsg,zc
python3 perfcat.py dummy 1350 47 1 47 sendmsg,zc
