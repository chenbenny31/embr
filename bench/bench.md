# BENCHMARKS.md — embr v0.7

All measurements taken on v0.7. Timing ends at page-cache write (no fsync) —
transfer, not durability. Consistent across all tools.

---

## Configuration

### WAN — AWS EC2 cross-region (us-east-1 → us-east-2)

```
sender:    c5n.large  us-east-1 (Virginia)   100.53.224.117
receiver:  c5n.large  us-east-2 (Ohio)        3.134.84.58
OS:        Amazon Linux 2023 (kernel 6.1)
embr:      v0.7, Release build (cmake -DCMAKE_BUILD_TYPE=Release)
file:      1 GB random (/dev/urandom), generated once, reused every run
sha256:    928e00addbc6ddec910b68c12082dce608d8eba226e9c8d9b2038fc06446ee67
cache:     page cache dropped on sender + receiver before each run
           (echo 3 > /proc/sys/vm/drop_caches)
runs:      10 measured + 2 warmup discarded
order:     interleaved embr → nc → scp per round
           (any cross-region condition drift hits all tools equally)
metrics:   receiver-side /usr/bin/time -v (wall, user, sys, maxRSS)
security:  inbound TCP 10007 (embr) + 9999 (nc) open on sender security group
note:      cross-region, not cross-AZ — RTT higher and more variable than
           same-region cross-AZ; document as cross-region in all references
```

### Loopback — Fedora Linux laptop

```
machine:   Fedora Linux (kernel 6.x), x86_64
embr:      v0.7, Debug build (cmake-build-debug)
file:      1 GB random (/dev/urandom), generated once, reused every run
sha256:    054c46417dd8859b465365bbb2dc84e6c9930c454090f782d92cf123ea0e8eb4
cache:     page cache dropped before each run
runs:      5 measured + 1 warmup discarded
order:     interleaved embr → nc → scp per round
metrics:   sender + receiver both timed via /usr/bin/time -v
purpose:   software overhead isolation — loopback has no RTT, buffer sizing
           is inert; measures CPU cost of the I/O path, not network throughput
```

---

## §1 — Zero-copy mechanism (strace syscall count)

**1 GB loopback transfer, syscall counts via `strace -c`.**
The mechanism story — not timing, which strace perturbs. Counts are faithful.

| process | dominant syscalls | total syscalls |
|---------|-------------------|----------------|
| embr push | `sendfile` ×1024 | ~7,408 |
| embr pull | `splice` ×4096 + `mmap` ×1024 (SHA-256 verify) | ~15,532 |
| ncat sender | `read` ×131,072 + `sendto` ×131,072 + `fcntl` ×524,288* | ~917,656 |
| ncat receiver | `recvfrom` ×131,072 + `write` ×131,072 | ~393,430 |

\* `fcntl` storm is a ncat implementation artifact, not protocol work.

**Bytes moved through userspace, transport path: embr = 0. ncat = 1 GB.**
This is the zero-copy claim shown directly at the syscall level — not a proxy,
not a statistic. embr's payload never enters a userspace buffer (`sendfile` on
push, `splice` on pull); ncat copies every byte through an 8 KB userspace
buffer (1 GB / 8 KB = 131,072 iterations) twice — once on each side.

embr push: 1 GB / 1 MB chunk = 1024 `sendfile` calls, file page-cache →
socket entirely in-kernel, zero payload reads or writes.

embr pull: `splice` ×4096 = 1024 chunks × ~4 splice iterations per chunk
(two hops: socket→pipe, pipe→file). `mmap` ×1024 is the SHA-256 verify pass
over received data — separate from transport, not a copy. Do not conflate.

**Do not headline raw syscall totals or ratios** — they are buffer-size and
implementation dependent (the fcntl storm inflates ncat; a different ncat
buffer size changes the counts). The bytes-through-userspace invariant is the
robust statement that survives any deep dive.

**strace is diagnosis, not measurement.** ptrace stops the process on every
syscall, distorting timing 10–100×. Never quote latency or throughput numbers
measured under strace. Use it to show what the program does; pair with perf
for how expensively.

---

## §2 — WAN cross-region transfer (1 GB, us-east-1 → us-east-2)

### Median summary (n=10, warmup excluded)

| tool | throughput | wall (median) | wall [min, max] | sys (median) | sys [min, max] |
|------|-----------|--------------|-----------------|-------------|----------------|
| embr | 1.17 Gbps | 7.33s | [7.30, 7.40] | 0.82s | [0.66, 0.88] |
| nc   | 1.17 Gbps | 7.33s | [7.30, 7.47] | 0.945s | [0.90, 1.01] |
| scp  | 0.83 Gbps | 10.41s | [10.04, 10.60] | 2.50s | — |

scp is SSH-encrypted — reference line only, never the headline comparison.
nc is the fair plaintext comparator (both tools, same plaintext path, only
difference is zero-copy vs copy-through-userspace).

### How to read this

**Throughput: embr == nc (1.17 Gbps both).** This is parity, not a win.
1.17 Gbps is the path ceiling (cross-region single-stream limit). nc hits
the same number. Do not claim "embr does 1.17 Gbps" as an embr result.

**The real differentiator is sys CPU:**
embr sys 0.82s < nc sys 0.945s, **13–15% lower kernel time, no overlap
across all 10 runs** (embr worst 0.88s < nc best 0.90s). This is the
splice/sendfile zero-copy signal — fewer kernel copies = less sys time.

**embr user time (2.58s) >> nc user time (0.25s).** This is receiver-side
SHA-256 verification (mmap + sha256_buf per chunk), which nc does not do.
1 GB / 2.58s ≈ 416 MB/s = software SHA-256 throughput (no SHA-NI on c5n).
It is integrity work, not a copy penalty.

**scp: 42% lower throughput.** Partly "we skip AES-128-CTR" — not an I/O
result. Keep as context ("real tool people use; encrypted, so not
apples-to-apples"), never as the primary comparison axis.

**Honest one-liner:** on a network-bound cross-region path, embr matches
raw netcat throughput while verifying end-to-end integrity, at 13–15% lower
kernel CPU. The zero-copy advantage becomes more dramatic on a fat, low-RTT
link where copy/CPU (not the network) is the bottleneck.

### Raw results

```
warmup1_embr wall=0:07.35 user=2.58s sys=0.83s rss=8480KB
warmup1_nc   wall=0:07.69 user=0.19s sys=1.01s rss=10080KB
warmup1_scp  wall=0:10.46 user=1.54s sys=2.26s rss=10080KB

warmup2_embr wall=0:07.33 user=2.58s sys=0.83s rss=8364KB
warmup2_nc   wall=0:07.45 user=0.28s sys=0.88s rss=10072KB
warmup2_scp  wall=0:10.06 user=1.30s sys=2.63s rss=10132KB

run3_embr  wall=0:07.33 user=2.50s sys=0.88s rss=8372KB
run3_nc    wall=0:07.31 user=0.31s sys=0.95s rss=10104KB
run3_scp   wall=0:10.37 user=1.53s sys=2.24s rss=10148KB

run4_embr  wall=0:07.33 user=2.61s sys=0.76s rss=8372KB
run4_nc    wall=0:07.45 user=0.26s sys=0.92s rss=10088KB
run4_scp   wall=0:10.04 user=1.57s sys=2.16s rss=10092KB

run5_embr  wall=0:07.40 user=2.59s sys=0.81s rss=8324KB
run5_nc    wall=0:07.33 user=0.27s sys=0.95s rss=10052KB
run5_scp   wall=0:10.46 user=1.50s sys=2.48s rss=10020KB

run6_embr  wall=0:07.30 user=2.53s sys=0.86s rss=8372KB
run6_nc    wall=0:07.37 user=0.24s sys=0.94s rss=10168KB
run6_scp   wall=0:10.48 user=1.41s sys=2.56s rss=10120KB

run7_embr  wall=0:07.33 user=2.56s sys=0.85s rss=8356KB
run7_nc    wall=0:07.32 user=0.23s sys=0.98s rss=10104KB
run7_scp   wall=0:10.09 user=1.42s sys=2.51s rss=10172KB

run8_embr  wall=0:07.31 user=2.58s sys=0.84s rss=8320KB
run8_nc    wall=0:07.30 user=0.20s sys=1.01s rss=10132KB
run8_scp   wall=0:10.22 user=1.25s sys=2.49s rss=10064KB

run9_embr  wall=0:07.34 user=2.72s sys=0.66s rss=8432KB
run9_nc    wall=0:07.47 user=0.25s sys=0.94s rss=10076KB
run9_scp   wall=0:10.51 user=1.33s sys=2.66s rss=10112KB

run10_embr wall=0:07.31 user=2.68s sys=0.72s rss=8372KB
run10_nc   wall=0:07.31 user=0.25s sys=0.93s rss=10080KB
run10_scp  wall=0:10.60 user=1.54s sys=2.28s rss=10008KB

run11_embr wall=0:07.33 user=2.59s sys=0.80s rss=8376KB
run11_nc   wall=0:07.34 user=0.30s sys=0.90s rss=10080KB
run11_scp  wall=0:10.05 user=1.41s sys=2.54s rss=10100KB

run12_embr wall=0:07.35 user=2.71s sys=0.72s rss=8324KB
run12_nc   wall=0:07.31 user=0.28s sys=0.99s rss=10180KB
run12_scp  wall=0:10.44 user=1.43s sys=2.54s rss=10080KB
```

---

## §3 — Loopback software overhead (1 GB, Fedora laptop)

### Median summary (n=5, warmup excluded)

| tool | throughput | send_user | send_sys | recv_user | recv_sys | total CPU |
|------|-----------|-----------|----------|-----------|----------|-----------|
| embr | 15.34 Gbps | 0.00s | 0.04s | 0.25s | 0.29s | 0.58s |
| nc   | 20.45 Gbps | 0.03s | 0.27s | 0.03s | 0.34s | 0.67s |
| scp  | 8.77 Gbps  | 0.00s | 0.00s | 0.27s | 0.71s | 0.98s |

scp is localhost SSH — encrypted, reference only.

### How to read this

**nc wins on loopback throughput (20.45 vs 15.34 Gbps).** This is expected
and honest. nc has no protocol overhead and no integrity verification.
embr's recv_user=0.25s is receiver-side SHA-256 per chunk — work nc does
not do at all. Do not claim embr beats nc on loopback throughput.

**The zero-copy signal is send_sys: embr 0.04s vs nc 0.27s — 6.75× lower
sender kernel time.** sendfile hands pages from the file's page cache
directly to the socket buffer; the sender process barely enters the kernel.
nc's sender calls `read()` to copy file data into a heap buffer, then
`send()` to copy it into the socket — two kernel crossings per chunk,
visible as high send_sys.

**embr total CPU = 0.58s vs nc total CPU = 0.67s** despite embr doing more
work (SHA-256 per chunk). The sendfile/splice zero-copy advantage on the
I/O path more than offsets the verification cost.

**loopback is the control, not the headline.** No RTT means buffer sizing
and BDP tuning are inert. The purpose here is to isolate the I/O path CPU
cost and demonstrate the zero-copy mechanism. WAN (§2) is the real-world
number.

### Raw results

```
warmup1_embr wall=0:00.54 send_user=0.00s send_sys=0.05s recv_user=0.25s recv_sys=0.26s
warmup1_nc   wall=0:00.42 send_user=0.04s send_sys=0.25s recv_user=0.03s recv_sys=0.33s
warmup1_scp  wall=0:01.05 send_user=0.00s send_sys=0.00s recv_user=0.27s recv_sys=0.71s

run2_embr wall=0:00.56 send_user=0.00s send_sys=0.04s recv_user=0.24s recv_sys=0.29s
run2_nc   wall=0:00.42 send_user=0.03s send_sys=0.28s recv_user=0.03s recv_sys=0.34s
run2_scp  wall=0:00.96 send_user=0.00s send_sys=0.00s recv_user=0.25s recv_sys=0.67s

run3_embr wall=0:00.54 send_user=0.00s send_sys=0.05s recv_user=0.25s recv_sys=0.26s
run3_nc   wall=0:00.42 send_user=0.03s send_sys=0.27s recv_user=0.03s recv_sys=0.34s
run3_scp  wall=0:01.00 send_user=0.00s send_sys=0.00s recv_user=0.30s recv_sys=0.68s

run4_embr wall=0:00.57 send_user=0.00s send_sys=0.04s recv_user=0.25s recv_sys=0.29s
run4_nc   wall=0:00.41 send_user=0.03s send_sys=0.23s recv_user=0.03s recv_sys=0.33s
run4_scp  wall=0:01.08 send_user=0.00s send_sys=0.00s recv_user=0.29s recv_sys=0.74s

run5_embr wall=0:00.57 send_user=0.00s send_sys=0.04s recv_user=0.24s recv_sys=0.29s
run5_nc   wall=0:00.44 send_user=0.03s send_sys=0.28s recv_user=0.04s recv_sys=0.34s
run5_scp  wall=0:00.94 send_user=0.00s send_sys=0.00s recv_user=0.27s recv_sys=0.72s

run6_embr wall=0:00.55 send_user=0.00s send_sys=0.05s recv_user=0.26s recv_sys=0.26s
run6_nc   wall=0:00.39 send_user=0.03s send_sys=0.25s recv_user=0.05s recv_sys=0.29s
run6_scp  wall=0:00.98 send_user=0.00s send_sys=0.00s recv_user=0.27s recv_sys=0.71s
```

---

## §4 — TCP socket tuning

```
CHUNK_SIZE:          1 MB (confirmed)
pipe size granted:   1048576 bytes (F_SETPIPE_SZ at CHUNK_SIZE, kernel granted exactly 1 MB)
fs.pipe-max-size:    1048576 (default Linux)
TCP_NODELAY:         on — prevents Nagle stall on CHUNK_REQ control messages
SO_SNDBUF:           not pinned — OS autotuning; bench sweep via tcp_from_fd
SO_RCVBUF:           not pinned — pinning disables receiver-side autotuning
SIGPIPE:             ignored process-wide (main.cpp) — sendfile has no MSG_NOSIGNAL
```

---

## §5 — Reproduction

**WAN:**
```bash
# sender (us-east-1 c5n.large) — open inbound TCP 10007 + 9999 in security group
ROLE=sender FILE_SIZE_GB=1 bash bench/tcp_wan.sh

# receiver (us-east-2 c5n.large)
ROLE=receiver SENDER_IP=<sender-ip> SSH_KEY=~/.ssh/key.pem bash bench/tcp_wan.sh
```

**Loopback:**
```bash
bash bench/tcp_loopback.sh
```

**strace syscall count:**
```bash
# terminal 1
strace -c ./embr push /tmp/bench.bin

# terminal 2
strace -c ./embr pull 127.0.0.1 --out /tmp/out.bin

# terminal 3 (nc baseline)
strace -c ncat -l 9999 --send-only < /tmp/bench.bin

# terminal 4
strace -c ncat 127.0.0.1 9999 --recv-only > /tmp/out_nc.bin
```