#!/bin/bash
cd "$(dirname "$0")"
echo "# RLIMIT_MEMLOCK = $(ulimit -l) KiB soft, $(ulimit -Hl) KiB hard; kernel $(uname -r); $(date)"
echo "# each test waits 1 s after the previous processes exit, so their charge is gone"
echo "## 1. one process, io_uring registered buffer: how much fits"
for k in 4096 6144 7168 8000 8100 8160 8180 8192 8200 8400; do ./memprobe 0 $k 0 one; sleep 1; done
echo "## 2. two processes of the same user, io_uring registered buffers"
./memprobe 0 5000 2500 A & sleep 0.5; ./memprobe 0 5000 0 "B while A holds 5000 KiB"; ./memprobe 0 3000 0 "B while A holds 5000 KiB"; wait; sleep 1
echo "## 3. another process holds 6 MiB with mlock(); does that reduce what io_uring can register?"
./memprobe 6144 0 2500 A & sleep 0.5; ./memprobe 0 6144 0 "B while A mlocks 6144 KiB"; ./memprobe 0 8000 0 "B while A mlocks 6144 KiB"; wait; sleep 1
echo "## 4. the same process: mlock() 6 MiB, then register 6 MiB with io_uring"
./memprobe 6144 6144 0 same; sleep 1
echo "## 5. mlock() alone: per process"
./memprobe 9000 0 0 "one process, 9000 KiB"; sleep 1
./memprobe 6144 0 2500 A & sleep 0.5; ./memprobe 6144 0 0 "B while A mlocks 6144 KiB"; wait; sleep 1
echo "## 6. charge after exit: register 5000 KiB in a process that exits at once, then again with no pause"
./memprobe 0 5000 0 first; ./memprobe 0 5000 0 "second, no pause"; ./memprobe 0 5000 0 "third, no pause"; sleep 1; ./memprobe 0 5000 0 "after 1 s"
