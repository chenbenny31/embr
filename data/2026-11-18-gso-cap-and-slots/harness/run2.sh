#!/bin/bash
cd "$(dirname "$0")"
echo "## 3b. repeat of 3 with a pause before the large registration"
./memprobe 6144 0 3000 A & sleep 1.2; ./memprobe 0 8000 0 "B while A mlocks 6144 KiB"; wait; sleep 1
echo "## 7. how long the charge outlives the process: register 5000 KiB, exit, wait, register 5000 KiB again"
for ms in 0 10 20 50 100 200 500; do ./memprobe 0 5000 0 first > /dev/null; sleep $(awk -v m=$ms 'BEGIN{printf "%.3f", m/1000}'); printf 'after %4s ms: ' $ms; ./memprobe 0 5000 0 again | sed 's/^again: //'; sleep 1; done
