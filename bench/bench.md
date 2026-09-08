# Benchmark measurements and raw data

The transfer campaign below was reported against the v0.8 tree (`9342d1e`,
Release build) on 2026-09-05/06. Transfer timing ends without an explicit fsync;
it measures transfer completion, not durable storage. Section 1 contains older
v0.7 syscall counts. The [corrected desktop ownership benchmark](#desktop-ownership-cost)
is a separate run after the benchmark implementation was repaired.

All six former `bench/results/` files are preserved verbatim in the
[raw-data archive](#raw-data-archive), including their original provenance and
superseded summary. Their filenames and SHA-256 digests identify the originals.
The corrected summaries and sample-selection rules below take precedence over
the historical live summaries inside raw blocks.

---

## Configuration

### WAN — AWS EC2 cross-region (us-east-1 → us-east-2)

```
sender:    c5n.large  us-east-1 (Virginia)
receiver:  c5n.large  us-east-2 (Ohio)
OS:        Amazon Linux 2023, kernel 6.18.44
embr:      v0.8 tree (9342d1e), Release build; ngtcp2 1.22.0 + wolfSSL 5.9.2
           (--enable-aesgcm-stream), liburing
file:      1 GiB random (/dev/urandom), generated once, pre-hashed once, reused
cache:     page cache dropped on sender + receiver before each run
runs:      10 measured + 2 warmup discarded
order:     interleaved per round: embr QUIC (SEND_ZC) → embr QUIC (sendmsg) →
           embr TCP → nc → scp, 2 s gap between rounds
metrics:   receiver-side /usr/bin/time -v around the pull (wall, user, sys, RSS);
           sender-side /usr/bin/time around each push (user, sys);
           EMBR_QUIC_STATS=1 on the SEND_ZC sender for the kernel's copy report
ports:     TCP 10007 (embr), 9999 (nc); UDP 10008 (QUIC SEND_ZC), 10009
           (QUIC sendmsg) inbound on the sender security group
preflight: kernel >= 6.2 and RLIMIT_MEMLOCK >= 1 MiB on both ends (SEND_ZC
           report-usage needs 6.2); net.core.{r,w}mem_max >= 4 MiB on both ends
note:      cross-region, not cross-AZ — RTT higher and more variable than
           same-region; document as cross-region in all references
```

### Loopback — Fedora desktop

```
machine:   AMD Ryzen 9 9900X (12 cores), Fedora Linux, kernel 7.1.13
embr:      v0.8 tree, Release build (-O3 -DNDEBUG)
file:      1 GiB random, generated once, reused every run
cache:     TCP rows (§3): page cache dropped before each run
           QUIC rows (§4): cache kept (cache=keep)
runs:      5 measured + 1 warmup discarded, interleaved per round
metrics:   sender + receiver both timed via /usr/bin/time -v
purpose:   single-machine throughput and CPU comparison; not a WAN result
```

---

## §1 — Zero-copy mechanism (strace syscall count)

**1 GiB loopback TCP transfer, syscall counts via `strace -c`.** Counted on
v0.7; these are historical mechanism counts, not a new v0.8 trace. Tracing can
perturb execution, so performance timings were obtained separately.

| process | dominant syscalls | total syscalls |
|---------|-------------------|----------------|
| embr push | `sendfile` ×1024 | ~7,408 |
| embr pull | `splice` ×4096 + `mmap` ×1024 (SHA-256 verify) | ~15,532 |
| ncat sender | `read` ×131,072 + `sendto` ×131,072 + `fcntl` ×524,288* | ~917,656 |
| ncat receiver | `recvfrom` ×131,072 + `write` ×131,072 | ~393,430 |

\* `fcntl` storm is a ncat implementation artifact, not protocol work.

**Payload bytes through a userspace buffer on the TCP transfer path: embr 0,
ncat 1 GiB (twice, once per side).** embr push: 1 GiB / 1 MiB chunk = 1024
`sendfile` calls, page cache → socket in-kernel. embr pull: `splice` ×4096 =
an average of four calls per chunk across two hops (socket → pipe → file). The `mmap`
×1024 is the SHA-256 verify pass over the written file — integrity work,
separate from the transport, not a copy.

This describes avoidance of application payload-buffer copies. It does not
establish that the kernel performs no internal copies; see [splice(2)](https://man7.org/linux/man-pages/man2/splice.2.html).

Do not headline raw syscall totals or ratios; they depend on buffer sizes and
on ncat internals. The bytes-through-userspace statement is the one that
survives a deep dive. strace is diagnosis, not measurement: never quote a
latency or throughput number taken under it.

---

## §2 — WAN cross-region transfer (1 GiB, us-east-1 → us-east-2)

### Receiver side, median [min, max], n = 10

| row | throughput | wall | user | sys |
|-----|-----------|------|------|-----|
| embr QUIC, SEND_ZC egress | 0.910 Gbps [0.89, 0.93] | 9.41 s [9.25, 9.65] | 5.74 s | 3.47 s |
| embr QUIC, sendmsg egress | 0.905 Gbps [0.88, 0.92] | 9.51 s [9.35, 9.74] | 5.81 s | 3.59 s |
| embr TCP (sendfile / splice) | 2.005 Gbps [1.81, 2.19] | 4.29 s [3.92, 4.75] | 2.64 s | 0.72 s |
| nc | 2.225 Gbps [2.00, 2.74] | 3.87 s [3.14, 4.29] | 0.27 s | 1.06 s |
| scp | 1.115 Gbps [1.04, 1.30] | 7.73 s [6.59, 8.27] | 0.96 s | 1.94 s |

### Sender side, median per push over the 10 timed pushes

| row | user | sys | datagrams | copied by the kernel |
|-----|------|-----|-----------|----------------------|
| embr QUIC, SEND_ZC egress | 2.77 s | 2.97 s | 11,243,123 over 12 connections | **0** |
| embr QUIC, sendmsg egress | 2.94 s | 2.56 s | — | all, by construction |
| embr TCP | 0.00 s | 0.32 s | — | — |

The first push of each row carries the sender's one-time file pre-hash
(user ≈ 7.4 s) and is a warmup. Brackets are min–max over the runs, a range,
not a confidence interval.

Sample selection: the receiver transcript contains repeated listings of the same
measurements. Deduplicate by `(run number, row)` and verify identical repeated
values; use run3 through run12 (ten per row). For each sender file, omit records
with nonzero exit status and the numeric line following `Command terminated by
signal 2`; then discard the first two successful records as warmups. Each sender
row has ten measured pushes. Sender wall time includes waiting for a connection
and is not a transfer-duration measurement.

The archived live sender summary and `wan_v08_summary.txt` incorrectly included
one interrupted process, reporting n=11 and user/sys medians 2.74/2.95 for SEND_ZC
and 2.93/2.56 for sendmsg. The table above uses the corrected ten samples:
unrounded user/sys medians are 2.765/2.970 and 2.940/2.560. Median total CPU is
computed from user+sys for each push: 5.755 s and 5.565 s, respectively. Summing
the separate component medians is not generally the median total.

### How to read this

**embr TCP vs nc: overlapping ranges, different medians.** 2.005 [1.81, 2.19]
against 2.225 [2.00, 2.74] Gbps. nc's throughput median is about 11% higher.
embr performs per-chunk hashing during transfer; receiver user CPU is 2.64 s
versus 0.27 s. Receiver kernel CPU is about 33% lower using unrounded medians
(0.715 versus 1.060 s). Overlapping ranges do not establish statistical
equivalence, and these process totals do not isolate copying or hashing costs.

**embr QUIC: similar throughput in the two sending modes.** SEND_ZC gives
0.910 Gbps and sendmsg 0.905 Gbps in this campaign. The median SEND_ZC receiver
total CPU is 9.23 s per GiB, computed from each run's user+sys, against 9.405 s
median elapsed. This is consistent with a CPU-limited receiver, but the logs
do not isolate hashing, decryption, packet handling or copying. Receive batching
and asynchronous file-write overlap are follow-up experiments.

**SEND_ZC did not reduce sender CPU in this campaign.** Its median total CPU
is 5.755 s per GiB versus 5.565 s for sendmsg. Component medians show about
0.41 s more kernel CPU and 0.175 s less user CPU. The terminal summary reports
0 copied among 11,243,123 sends, with no fallback or error, over 12 connections
including warmups. The individual egress logs were not retained; the aggregate
is preserved evidence but cannot be independently recomputed here. The data do
not establish the cost of completion processing or the percentage spent copying.
Segmentation offload is a proposed experiment, not a demonstrated improvement.

**scp is the encrypted reference, not the headline.** 1.12 Gbps against
embr QUIC's 0.91: scp decrypts large records over a kernel-reassembled TCP
stream at 2.9 s of receiver CPU per GiB; embr QUIC handles one 1350-byte
datagram at a time at 9.2 s. Different work; keep it as context.

### Raw results

The [raw-data archive](#raw-data-archive) contains the complete receiver terminal
output, sender per-push files, provenance and historical summary. Selected timed
receiver lines are also reproduced here for readability:

```
run3_embrquic    wall=0:09.36 gbps=0.92 user=5.67s sys=3.52s
run3_embrquicsm  wall=0:09.41 gbps=0.91 user=5.58s sys=3.65s
run3_embrtcp     wall=0:04.13 gbps=2.08 user=2.66s sys=0.71s
run3_nc          wall=0:04.29 gbps=2.00 user=0.29s sys=1.12s
run3_scp         wall=0:07.70 gbps=1.12 user=0.96s sys=2.03s

run4_embrquic    wall=0:09.41 gbps=0.91 user=5.44s sys=3.78s
run4_embrquicsm  wall=0:09.47 gbps=0.91 user=6.03s sys=3.29s
run4_embrtcp     wall=0:04.01 gbps=2.14 user=2.54s sys=0.79s
run4_nc          wall=0:04.14 gbps=2.07 user=0.15s sys=1.24s
run4_scp         wall=0:06.73 gbps=1.28 user=0.85s sys=1.87s

run5_embrquic    wall=0:09.65 gbps=0.89 user=5.71s sys=3.77s
run5_embrquicsm  wall=0:09.39 gbps=0.91 user=6.20s sys=3.00s
run5_embrtcp     wall=0:04.08 gbps=2.11 user=2.46s sys=0.89s
run5_nc          wall=0:03.58 gbps=2.40 user=0.21s sys=1.19s
run5_scp         wall=0:07.93 gbps=1.08 user=0.92s sys=1.99s

run6_embrquic    wall=0:09.29 gbps=0.92 user=6.07s sys=3.05s
run6_embrquicsm  wall=0:09.35 gbps=0.92 user=5.54s sys=3.66s
run6_embrtcp     wall=0:04.66 gbps=1.84 user=2.67s sys=0.64s
run6_nc          wall=0:03.91 gbps=2.20 user=0.19s sys=1.07s
run6_scp         wall=0:08.27 gbps=1.04 user=0.89s sys=2.07s

run7_embrquic    wall=0:09.25 gbps=0.93 user=6.18s sys=2.92s
run7_embrquicsm  wall=0:09.43 gbps=0.91 user=5.93s sys=3.33s
run7_embrtcp     wall=0:04.45 gbps=1.93 user=2.76s sys=0.54s
run7_nc          wall=0:04.12 gbps=2.08 user=0.26s sys=1.05s
run7_scp         wall=0:07.75 gbps=1.11 user=0.95s sys=1.86s

run8_embrquic    wall=0:09.44 gbps=0.91 user=5.96s sys=3.32s
run8_embrquicsm  wall=0:09.56 gbps=0.90 user=5.82s sys=3.55s
run8_embrtcp     wall=0:04.10 gbps=2.10 user=2.61s sys=0.75s
run8_nc          wall=0:04.18 gbps=2.06 user=0.31s sys=1.04s
run8_scp         wall=0:07.75 gbps=1.11 user=0.95s sys=1.94s

run9_embrquic    wall=0:09.36 gbps=0.92 user=5.76s sys=3.41s
run9_embrquicsm  wall=0:09.55 gbps=0.90 user=5.54s sys=3.82s
run9_embrtcp     wall=0:04.56 gbps=1.88 user=2.85s sys=0.49s
run9_nc          wall=0:03.64 gbps=2.36 user=0.27s sys=0.89s
run9_scp         wall=0:07.02 gbps=1.22 user=1.03s sys=1.94s

run10_embrquic   wall=0:09.40 gbps=0.91 user=5.57s sys=3.67s
run10_embrquicsm wall=0:09.54 gbps=0.90 user=5.80s sys=3.58s
run10_embrtcp    wall=0:04.75 gbps=1.81 user=2.50s sys=0.85s
run10_nc         wall=0:03.82 gbps=2.25 user=0.31s sys=0.84s
run10_scp        wall=0:06.59 gbps=1.30 user=1.02s sys=1.92s

run11_embrquic   wall=0:09.46 gbps=0.91 user=6.04s sys=3.27s
run11_embrquicsm wall=0:09.67 gbps=0.89 user=5.89s sys=3.59s
run11_embrtcp    wall=0:04.55 gbps=1.89 user=2.74s sys=0.59s
run11_nc         wall=0:03.70 gbps=2.32 user=0.31s sys=1.00s
run11_scp        wall=0:06.61 gbps=1.30 user=1.10s sys=1.82s

run12_embrquic   wall=0:09.64 gbps=0.89 user=5.58s sys=3.88s
run12_embrquicsm wall=0:09.74 gbps=0.88 user=5.63s sys=3.93s
run12_embrtcp    wall=0:03.92 gbps=2.19 user=2.62s sys=0.72s
run12_nc         wall=0:03.14 gbps=2.74 user=0.27s sys=1.13s
run12_scp        wall=0:07.89 gbps=1.09 user=0.98s sys=1.98s
```

Sender egress counters as reported by the archived terminal summary over 12 connections:
`sends=11243123 copied=0 (0.0%) fallback=0 errors=0 registered`.

---

## §3 — Loopback TCP (1 GiB, Release build, caches dropped)

### Median summary (n = 5, warmup excluded)

| tool | throughput | wall | send_user | send_sys | recv_user | recv_sys |
|------|-----------|------|-----------|----------|-----------|----------|
| embr | 12.82 Gbps | 0.67 s | 0.00 s | 0.03 s | 0.40 s | 0.25 s |
| nc   | 27.71 Gbps | 0.31 s | 0.02 s | 0.19 s | 0.02 s | 0.23 s |
| scp  | 16.52 Gbps | 0.52 s | 0.00 s | 0.00 s | 0.12 s | 0.40 s |

### How to read this

**nc finishes first (0.31 s vs 0.67 s).** embr also handles control messages
and per-chunk SHA-256 checks. Its 0.40 s receiver user CPU covers all userspace
work; hashing was not timed separately. This comparison does not isolate the
cost of integrity checking.

**Sender kernel CPU: embr 0.03 s vs nc 0.19 s, about 84% lower.** These are
measurements of the complete sendfile and ncat send paths. They do not isolate
payload-copy costs from syscall frequency or other kernel work.

**scp on loopback is localhost SSH** — the sender's CPU is charged to sshd,
not to the timed process, so its 0.00 s sender columns are an accounting
artifact. Reference only.

### Raw results

```
run2_embr wall=0:00.67 gbps=12.82 send_user=0.00s send_sys=0.03s recv_user=0.40s recv_sys=0.24s
run2_nc   wall=0:00.28 gbps=30.68 send_user=0.01s send_sys=0.19s recv_user=0.02s recv_sys=0.23s
run2_scp  wall=0:00.48 gbps=17.90 send_user=0.00s send_sys=0.00s recv_user=0.11s recv_sys=0.37s

run3_embr wall=0:00.67 gbps=12.82 send_user=0.00s send_sys=0.03s recv_user=0.39s recv_sys=0.26s
run3_nc   wall=0:00.32 gbps=26.84 send_user=0.02s send_sys=0.21s recv_user=0.03s recv_sys=0.26s
run3_scp  wall=0:00.51 gbps=16.84 send_user=0.00s send_sys=0.00s recv_user=0.11s recv_sys=0.39s

run4_embr wall=0:00.66 gbps=13.02 send_user=0.00s send_sys=0.03s recv_user=0.40s recv_sys=0.23s
run4_nc   wall=0:00.34 gbps=25.26 send_user=0.02s send_sys=0.23s recv_user=0.02s recv_sys=0.28s
run4_scp  wall=0:00.60 gbps=14.32 send_user=0.00s send_sys=0.00s recv_user=0.12s recv_sys=0.52s

run5_embr wall=0:00.70 gbps=12.27 send_user=0.00s send_sys=0.04s recv_user=0.41s recv_sys=0.27s
run5_nc   wall=0:00.31 gbps=27.71 send_user=0.01s send_sys=0.17s recv_user=0.02s recv_sys=0.23s
run5_scp  wall=0:00.52 gbps=16.52 send_user=0.00s send_sys=0.00s recv_user=0.12s recv_sys=0.40s

run6_embr wall=0:00.67 gbps=12.82 send_user=0.00s send_sys=0.03s recv_user=0.38s recv_sys=0.25s
run6_nc   wall=0:00.26 gbps=33.04 send_user=0.02s send_sys=0.18s recv_user=0.02s recv_sys=0.22s
run6_scp  wall=0:00.53 gbps=16.21 send_user=0.00s send_sys=0.00s recv_user=0.12s recv_sys=0.41s
```

---

## §4 — Loopback QUIC (1 GiB, Release build, cache kept)

### Median summary (n = 5, warmup excluded)

| row | throughput | wall | send_user | send_sys | recv_user | recv_sys |
|-----|-----------|------|-----------|----------|-----------|----------|
| embr TCP | 12.82 Gbps | 0.67 s | 0.00 s | 0.03 s | 0.38 s | 0.27 s |
| embr QUIC, SEND_ZC egress | 3.21 Gbps | 2.68 s | 0.72 s | 1.11 s | 1.12 s | 1.14 s |
| embr QUIC, sendmsg egress | 3.55 Gbps | 2.42 s | 0.71 s | 0.99 s | 1.02 s | 1.04 s |
| nc | 28.63 Gbps | 0.30 s | 0.01 s | 0.19 s | 0.02 s | 0.24 s |

### How to read this

**The loopback campaign reported copying for every SEND_ZC send.** The reported
egress counters were `copied == sends`; individual counter logs are not included
in the available archive. SEND_ZC measured 0.12 s more sender kernel CPU and
0.34 Gbps less throughput than sendmsg here. This is not evidence of copy
avoidance, nor does the timing isolate notification overhead.

**QUIC vs TCP on loopback: encrypted vs plaintext.** Both elapsed and CPU
measurements describe different protocol and processing work. This comparison
does not isolate the cost of the ownership representation; the separate desktop
microbenchmark below exercises the owner operations directly.

### Raw results

```
run2_embrtcp    wall=0:00.67 gbps=12.82 send_user=0.00s send_sys=0.03s recv_user=0.38s recv_sys=0.27s
run2_embrquic   wall=0:02.68 gbps=3.21  send_user=0.72s send_sys=1.09s recv_user=1.12s recv_sys=1.11s
run2_embrquicsm wall=0:02.42 gbps=3.55  send_user=0.70s send_sys=0.99s recv_user=1.08s recv_sys=0.98s
run2_nc         wall=0:00.30 gbps=28.63 send_user=0.02s send_sys=0.20s recv_user=0.02s recv_sys=0.25s

run3_embrtcp    wall=0:00.67 gbps=12.82 send_user=0.00s send_sys=0.03s recv_user=0.39s recv_sys=0.25s
run3_embrquic   wall=0:02.69 gbps=3.19  send_user=0.71s send_sys=1.12s recv_user=1.09s recv_sys=1.14s
run3_embrquicsm wall=0:02.36 gbps=3.64  send_user=0.69s send_sys=0.95s recv_user=0.97s recv_sys=1.03s
run3_nc         wall=0:00.30 gbps=28.63 send_user=0.01s send_sys=0.19s recv_user=0.02s recv_sys=0.24s

run4_embrtcp    wall=0:00.67 gbps=12.82 send_user=0.00s send_sys=0.03s recv_user=0.39s recv_sys=0.25s
run4_embrquic   wall=0:02.78 gbps=3.09  send_user=0.73s send_sys=1.15s recv_user=1.15s recv_sys=1.16s
run4_embrquicsm wall=0:02.41 gbps=3.56  send_user=0.72s send_sys=0.96s recv_user=1.01s recv_sys=1.04s
run4_nc         wall=0:00.34 gbps=25.26 send_user=0.01s send_sys=0.23s recv_user=0.02s recv_sys=0.28s

run5_embrtcp    wall=0:00.67 gbps=12.82 send_user=0.00s send_sys=0.03s recv_user=0.37s recv_sys=0.27s
run5_embrquic   wall=0:02.68 gbps=3.21  send_user=0.71s send_sys=1.11s recv_user=1.07s recv_sys=1.14s
run5_embrquicsm wall=0:02.49 gbps=3.45  send_user=0.73s send_sys=1.01s recv_user=1.05s recv_sys=1.07s
run5_nc         wall=0:00.30 gbps=28.63 send_user=0.01s send_sys=0.18s recv_user=0.02s recv_sys=0.23s

run6_embrtcp    wall=0:00.68 gbps=12.63 send_user=0.00s send_sys=0.03s recv_user=0.38s recv_sys=0.27s
run6_embrquic   wall=0:02.68 gbps=3.21  send_user=0.74s send_sys=1.09s recv_user=1.13s recv_sys=1.11s
run6_embrquicsm wall=0:02.44 gbps=3.52  send_user=0.71s send_sys=0.99s recv_user=1.02s recv_sys=1.05s
run6_nc         wall=0:00.31 gbps=27.71 send_user=0.02s send_sys=0.16s recv_user=0.02s recv_sys=0.23s
```

---

## §5 — Transport settings

### TCP

```
CHUNK_SIZE:          1 MiB
pipe size granted:   1048576 bytes (F_SETPIPE_SZ at CHUNK_SIZE)
TCP_NODELAY:         on — prevents Nagle stall on CHUNK_REQ control messages
SO_SNDBUF/SO_RCVBUF: not pinned — OS autotuning
SIGPIPE:             ignored process-wide — sendfile has no MSG_NOSIGNAL
```

### QUIC / SEND_ZC egress

```
datagram:            QUIC_MAX_PKTLEN = 1350 bytes (not tuned; conservative under a 1500 MTU)
egress slab:         QUIC_ZC_SLOTS = 64 slots × 1350 B per connection, registered once;
                     about 105 KiB locked per connection including the ring
completion rule:     send CQE with F_MORE → hold the slot until NOTIF; without F_MORE → final
fallbacks:           refused datagram (ENOMEM/ENOBUFS/EAGAIN/EINVAL/EOPNOTSUPP) copy-sent once;
                     no registration or kernel < 6.2 → sendmsg for the connection;
                     slab still held at teardown → leaked, never reused
knobs:               EMBR_QUIC_EGRESS=sendmsg (copying baseline), EMBR_QUIC_STATS=1
                     (per-connection sends/notifs/copied/fallback/errors at close)
flow control:        4 MiB per stream, 8 MiB per connection, one bidi stream
receive loop:        select + one recvmsg per datagram (v0.8); recvmmsg batching is v0.9
certificate:         client does not verify the server (SSL_VERIFY_NONE) in v0.8
```

---

## §6 — Reproduction

Run these commands from the repository root. Use a Release binary in `./build`
for each harness; the directory name alone does not establish the build type.

**Dependencies and Release build (both WAN hosts, or the loopback host):**

```bash
BUILD_EMBR=1 EMBR_BUILD_DIR=./build bash bench/setup_quic_deps.sh
```

The setup script installs dependencies, updates UDP socket limits, checks
kernel/memlock prerequisites and builds embr. With dependencies already prepared,
use the [README Release build commands](../README.md#build).

**WAN, all five rows:**

```bash
# sender (us-east-1) — open inbound TCP 10007, 9999 and UDP 10008, 10009
ROLE=sender BUILD=./build FILE_SIZE_GB=1 bash bench/quic_wan.sh

# receiver (us-east-2)
ROLE=receiver BUILD=./build SENDER_IP="<sender-ip>" SSH_KEY=~/.ssh/key.pem bash bench/quic_wan.sh
```
`bench/tcp_wan.sh` runs the TCP-only subset with the same harness.

**Loopback:**

```bash
BUILD=./build bash bench/tcp_loopback.sh            # embr TCP, nc, scp; cache=drop
BUILD=./build CACHE_DISCIPLINE=keep \
  CERT=/path/to/cert.pem KEY=/path/to/key.pem \
  bash bench/quic_loopback.sh                      # embr TCP, QUIC SEND_ZC, QUIC sendmsg, nc
```

Replace the certificate/key paths with an existing test pair; the QUIC loopback
script does not generate one. The TCP loopback scp reference requires a working
localhost SSH login.

Cache-drop helpers suppress failures. For another cold-cache campaign, verify
that both hosts permit the cache drop; `cache=drop` in a log only records the
requested setting.

**strace syscall count (§1):**

With a prepared 1 GiB `/tmp/bench.bin`, these commands trace the current build.
The counts retained in §1 describe the historical v0.7 run.

```bash
strace -c ./build/embr push /tmp/bench.bin --transport tcp                 # terminal 1
strace -c ./build/embr pull 127.0.0.1 --transport tcp --out /tmp/out.bin   # terminal 2
strace -c ncat -l 9999 --send-only < /tmp/bench.bin    # terminal 3 (nc baseline)
strace -c ncat 127.0.0.1 9999 --recv-only > /tmp/out_nc.bin   # terminal 4
```

---

<a id="desktop-ownership-cost"></a>
## §7 — Corrected desktop ownership benchmark

This is the selected ownership-cost dataset for the poster. The old 11.5/19.5 ns
Buffer estimates and their 0.7/1.2 ns function-pointer comparisons are superseded:
the former RawBuf comparator used copy assignment and released its temporary
before the retained table entry was retired. There is no corrected c5n result.

| Representation | Median ns/lifetime | Min–max ns/lifetime | Object size |
|---|---:|---:|---:|
| Buffer / std::function | 15.552 | 15.366–17.313 | 56 B |
| RawBuf / function pointer with inline context | 1.649 | 1.635–1.660 | 40 B |
| Paired difference (Buffer − RawBuf) | 13.900 | 13.718–15.671 | — |

Environment: AMD Ryzen 9 9900X desktop (12 cores / 24 threads), Fedora Linux,
kernel 7.1.13-200.fc44.x86_64, GCC 16.2.1 20260819 (Red Hat 16.2.1-2),
libstdc++ 20260819. This local validation run used the corrected
bench/erasure_cost.cpp with -std=c++20 -O3 -DNDEBUG on 2026-09-06.
The process was not pinned to a CPU and frequency was not fixed.

Both owners carry a pool pointer and a slot index, move into a 64-entry table,
remain materialized across a GNU compiler barrier, retire, return the slot,
expose the release output to a second barrier, then clear the free list.
The pool is preallocated. Each owner receives one million warmup lifetimes
(excluded), followed by ten rounds of 20 million lifetimes per owner.
Execution order alternates between rounds. All rounds reported zero intercepted
ordinary scalar operator new calls; this is not a general allocation guarantee.

The table reports the executable's summaries, computed before printing rounded
per-round values. Recomputing from the three-decimal raw output can differ in the
last digit. The paired difference is summarized per round, not obtained by simply
subtracting the displayed medians.

These are whole-owner loop measurements, including representation, compiler-barrier
and cleanup work. GCC retained the owner/release operations and specialized known
raw callback targets with guarded direct calls. The results do not isolate indirect
dispatch, apply to arbitrary closures, or establish production CPU percentages.
No per-GiB extrapolation is made.

Both owners passed active checks for move construction, replacement of an occupied
destination, moved-from destruction, repeated retirement and scope-exit cleanup.
Release and ASan/UBSan preflight runs passed. See the
[implementation and validation record](../docs/tasks/erasure-cost-benchmark.md).

### Reproduce locally

From the repository root, using the existing compiler and project headers:

```bash
c++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Isrc bench/erasure_cost.cpp -o /tmp/embr-erasure-cost-bench
/tmp/embr-erasure-cost-bench --check-only
/tmp/embr-erasure-cost-bench
```

### Complete desktop output

### desktop-erasure-cost.txt

Original bytes: 1728; SHA-256: 54d648174b33b7dcf6d71de70c3460824d20875acb39f20b3e8daf5a22db569b.

<!-- raw-begin:desktop-erasure-cost.txt -->
````text
Buffer lifetime checks: PASS
RawBuf lifetime checks: PASS
Compiler: 16.2.1 20260819 (Red Hat 16.2.1-2)
libstdc++ date: 20260819
sizeof(Buffer)=56 sizeof(RawBuf)=40
10 rounds, 20000000 lifetimes/owner/round; alternating order
Warmup: 1000000 lifetimes per owner (excluded)
round=1 order=Buffer,RawBuf Buffer_ns=17.313 RawBuf_ns=1.642 Buffer_new_calls=0 RawBuf_new_calls=0
round=2 order=RawBuf,Buffer Buffer_ns=15.436 RawBuf_ns=1.660 Buffer_new_calls=0 RawBuf_new_calls=0
round=3 order=Buffer,RawBuf Buffer_ns=15.366 RawBuf_ns=1.641 Buffer_new_calls=0 RawBuf_new_calls=0
round=4 order=RawBuf,Buffer Buffer_ns=15.474 RawBuf_ns=1.635 Buffer_new_calls=0 RawBuf_new_calls=0
round=5 order=Buffer,RawBuf Buffer_ns=15.527 RawBuf_ns=1.653 Buffer_new_calls=0 RawBuf_new_calls=0
round=6 order=RawBuf,Buffer Buffer_ns=15.713 RawBuf_ns=1.644 Buffer_new_calls=0 RawBuf_new_calls=0
round=7 order=Buffer,RawBuf Buffer_ns=15.621 RawBuf_ns=1.651 Buffer_new_calls=0 RawBuf_new_calls=0
round=8 order=RawBuf,Buffer Buffer_ns=15.598 RawBuf_ns=1.649 Buffer_new_calls=0 RawBuf_new_calls=0
round=9 order=Buffer,RawBuf Buffer_ns=15.576 RawBuf_ns=1.650 Buffer_new_calls=0 RawBuf_new_calls=0
round=10 order=RawBuf,Buffer Buffer_ns=15.378 RawBuf_ns=1.660 Buffer_new_calls=0 RawBuf_new_calls=0
Buffer / std::function           median 15.552 ns/lifetime, range [15.366, 17.313]
RawBuf / function pointer        median 1.649 ns/lifetime, range [1.635, 1.660]
Paired difference (Buffer - Raw) median 13.900 ns/lifetime, range [13.718, 15.671]
Whole-owner loop measurements, including compiler barriers and release work.
new_calls counts intercepted ordinary scalar operator new calls only.
No production-overhead percentage or per-GiB extrapolation is inferred.
````
<!-- raw-end:desktop-erasure-cost.txt -->


---

<a id="raw-data-archive"></a>
## §8 — Consolidated WAN source files

The following six files were copied byte-for-byte from bench/results/ before
their originals were removed. Original names and hashes are retained for recovery.
The full transcript includes warmups, two listings of the same measured receiver
runs, and live summaries. Repeated listings are not additional samples.

wan_v08_summary.txt and the live sender summary contain superseded n=11 sender
statistics. Use §2's corrected n=10 selection and medians. The per-push sender
file format is wall_seconds user_seconds sys_seconds exit_status; signal/error
diagnostics describe the following numeric record. Keep those diagnostics when
recomputing statistics. The egress aggregate is present in the terminal output,
but the individual .embrquic.log source was never copied from the host.

### PROVENANCE.txt

Original bytes: 640; SHA-256: 0f936727e8cde9348b2a318b78f2c1c408f8c70f47063dda11f6e26f2e2bda03.

<!-- raw-begin:PROVENANCE.txt -->
````text
WAN campaign 2026-09-06, c5n.large us-east-1 -> us-east-2, AL2023 kernel 6.18.44, 1 GiB, n=10 timed + 2 warmups.
bench_quic_wan_receiver.terminal.txt : the receiver's terminal output as pasted by the author on 2026-09-06 (10 run markers,
                                       0 'attempt N failed' lines). The host's raw file was /tmp/bench_quic_wan_results.txt.
bench_quic_wan_sender.embrquic/.embrquicsm/.embrtcp : the sender's per-push /usr/bin/time lines (host: /tmp/bench_quic_wan_sender.<row>).
wan_v08_summary.txt : medians as used in README.md and POSTER-V08.md. The sender's egress log (.embrquic.log) was not copied off the host.
````
<!-- raw-end:PROVENANCE.txt -->

### wan_v08_summary.txt

Original bytes: 732; SHA-256: 14ff1513465a3b913512848a5b034a7be34bbdde61d57f79892fc40d4e5b62b1.

<!-- raw-begin:wan_v08_summary.txt -->
````text
WAN 2026-09-06 21:55-22:07 UTC, c5n.large us-east-1 -> us-east-2, AL2023 kernel 6.18.44, 1 GiB, 10 runs + 2 warmups
receiver (pull side, /usr/bin/time -v) median [min..max]:
embrquic   0.91 Gbps [0.89 0.93]  wall 9.41 s  user 5.74  sys 3.47
embrquicsm 0.905     [0.88 0.92]  wall 9.51 s  user 5.81  sys 3.59
embrtcp    2.005     [1.81 2.19]  wall 4.29 s  user 2.64  sys 0.72
nc         2.225     [2.00 2.74]  wall 3.87 s  user 0.27  sys 1.06
scp        1.115     [1.04 1.30]  wall 7.73 s  user 0.96  sys 1.94
sender (push side, per push, n=11 medians): embrquic user 2.74 sys 2.95 | embrquicsm user 2.93 sys 2.56 | embrtcp user 0.00 sys 0.32
egress over 12 connections: sends=11243123 copied=0 (0.0%) fallback=0 errors=0 registered
````
<!-- raw-end:wan_v08_summary.txt -->

### bench_quic_wan_receiver.terminal.txt

Original bytes: 12908; SHA-256: 23993a232fd8ce8e0bc43ea76b3c5afc5ade32fab193a275d12c92709a78959e.

<!-- raw-begin:bench_quic_wan_receiver.terminal.txt -->
````text
^C[bench_quic_wan 22:07:44] === sender summary (median over completed pushes, warmups excluded) ===

row        | n      | user s   | sys s   
embrquic   | 11     | 2.74     | 2.95    
embrquicsm | 11     | 2.93     | 2.56    
embrtcp    | 11     | 0.00     | 0.32    
embrquic egress over 12 connections: sends=11243123 copied=0 (0.0%) fallback=0 errors=0
[bench_quic_wan 22:07:44] raw per-push lines in /tmp/bench_quic_wan_sender.<row>; read: embrquic sys vs embrquicsm sys = SEND_ZC saving per GiB
 [ec2-user@ip-172-31-47-172 embr]$   ROLE=receiver SENDER_IP=44.198.164.219 SSH_KEY=~/.ssh/key.pem bash bench/quic_wan.sh
[bench_quic_wan 21:55:31] ssh ok: ec2-user@44.198.164.219
[bench_quic_wan 21:55:31] tcp ports ok: 10007 9999 (udp 10008 10009 are probed by the first warmup)
[bench_quic_wan 21:55:31] local sockbuf ok: rmem_max=16777216 wmem_max=16777216
[bench_quic_wan 21:55:32] remote sockbuf ok: rmem_max=16777216 wmem_max=16777216
[bench_quic_wan 21:55:32] local sendzc ok: kernel=6.18.44-99.149.amzn2023.x86_64 memlock=unlimitedKiB
[bench_quic_wan 21:55:33] remote sendzc ok: kernel=6.18.44-99.149.amzn2023.x86_64 memlock=unlimitedKiB
[bench_quic_wan 21:55:33] RECEIVER — ec2-user@44.198.164.219 — fetching source size+hash...
[bench_quic_wan 21:55:36] expected 1073741824 bytes  sha256=373680e860d75ca8af4f7e257831aea1150834624d87f74c802b6c9efb705b36
[bench_quic_wan 21:55:36] === warmup1 (1/12) ===
warmup1_embrquic wall=0:09.64 sec=9.64 gbps=0.89 user=5.83s sys=3.64s rss=11964KB
warmup1_embrquicsm wall=0:09.60 sec=9.6 gbps=0.89 user=5.51s sys=3.93s rss=12040KB
warmup1_embrtcp wall=0:04.35 sec=4.35 gbps=1.97 user=2.60s sys=0.70s rss=9536KB
warmup1_nc wall=0:04.28 sec=4.28 gbps=2.01 user=0.26s sys=1.05s rss=9668KB
warmup1_scp wall=0:07.75 sec=7.75 gbps=1.11 user=0.93s sys=2.03s rss=11216KB
[bench_quic_wan 21:56:31] === warmup2 (2/12) ===
warmup2_embrquic wall=0:09.42 sec=9.42 gbps=0.91 user=5.97s sys=3.29s rss=11896KB
warmup2_embrquicsm wall=0:09.51 sec=9.51 gbps=0.90 user=5.71s sys=3.64s rss=12032KB
warmup2_embrtcp wall=0:04.15 sec=4.15 gbps=2.07 user=2.62s sys=0.72s rss=9528KB
warmup2_nc wall=0:04.31 sec=4.31 gbps=1.99 user=0.28s sys=1.07s rss=9720KB
warmup2_scp wall=0:08.07 sec=8.07 gbps=1.06 user=1.07s sys=1.85s rss=11188KB
[bench_quic_wan 21:57:26] === run3 (3/12) ===
run3_embrquic wall=0:09.36 sec=9.36 gbps=0.92 user=5.67s sys=3.52s rss=11912KB
run3_embrquicsm wall=0:09.41 sec=9.41 gbps=0.91 user=5.58s sys=3.65s rss=11984KB
run3_embrtcp wall=0:04.13 sec=4.13 gbps=2.08 user=2.66s sys=0.71s rss=9564KB
run3_nc wall=0:04.29 sec=4.29 gbps=2.00 user=0.29s sys=1.12s rss=9740KB
run3_scp wall=0:07.70 sec=7.7 gbps=1.12 user=0.96s sys=2.03s rss=11076KB
[bench_quic_wan 21:58:21] === run4 (4/12) ===
run4_embrquic wall=0:09.41 sec=9.41 gbps=0.91 user=5.44s sys=3.78s rss=12064KB
run4_embrquicsm wall=0:09.47 sec=9.47 gbps=0.91 user=6.03s sys=3.29s rss=11980KB
run4_embrtcp wall=0:04.01 sec=4.01 gbps=2.14 user=2.54s sys=0.79s rss=9560KB
run4_nc wall=0:04.14 sec=4.14 gbps=2.07 user=0.15s sys=1.24s rss=9760KB
run4_scp wall=0:06.73 sec=6.73 gbps=1.28 user=0.85s sys=1.87s rss=10880KB
[bench_quic_wan 21:59:14] === run5 (5/12) ===
run5_embrquic wall=0:09.65 sec=9.65 gbps=0.89 user=5.71s sys=3.77s rss=11916KB
run5_embrquicsm wall=0:09.39 sec=9.39 gbps=0.91 user=6.20s sys=3.00s rss=12008KB
run5_embrtcp wall=0:04.08 sec=4.08 gbps=2.11 user=2.46s sys=0.89s rss=9536KB
run5_nc wall=0:03.58 sec=3.58 gbps=2.40 user=0.21s sys=1.19s rss=9648KB
run5_scp wall=0:07.93 sec=7.93 gbps=1.08 user=0.92s sys=1.99s rss=10712KB
[bench_quic_wan 22:00:08] === run6 (6/12) ===
run6_embrquic wall=0:09.29 sec=9.29 gbps=0.92 user=6.07s sys=3.05s rss=11948KB
run6_embrquicsm wall=0:09.35 sec=9.35 gbps=0.92 user=5.54s sys=3.66s rss=12032KB
run6_embrtcp wall=0:04.66 sec=4.66 gbps=1.84 user=2.67s sys=0.64s rss=9636KB
run6_nc wall=0:03.91 sec=3.91 gbps=2.20 user=0.19s sys=1.07s rss=9548KB
run6_scp wall=0:08.27 sec=8.27 gbps=1.04 user=0.89s sys=2.07s rss=10788KB
[bench_quic_wan 22:01:03] === run7 (7/12) ===
run7_embrquic wall=0:09.25 sec=9.25 gbps=0.93 user=6.18s sys=2.92s rss=12052KB
run7_embrquicsm wall=0:09.43 sec=9.43 gbps=0.91 user=5.93s sys=3.33s rss=12064KB
run7_embrtcp wall=0:04.45 sec=4.45 gbps=1.93 user=2.76s sys=0.54s rss=9560KB
run7_nc wall=0:04.12 sec=4.12 gbps=2.08 user=0.26s sys=1.05s rss=9796KB
run7_scp wall=0:07.75 sec=7.75 gbps=1.11 user=0.95s sys=1.86s rss=11092KB
[bench_quic_wan 22:01:58] === run8 (8/12) ===
run8_embrquic wall=0:09.44 sec=9.44 gbps=0.91 user=5.96s sys=3.32s rss=11896KB
run8_embrquicsm wall=0:09.56 sec=9.56 gbps=0.90 user=5.82s sys=3.55s rss=11912KB
run8_embrtcp wall=0:04.10 sec=4.1 gbps=2.10 user=2.61s sys=0.75s rss=9552KB
run8_nc wall=0:04.18 sec=4.18 gbps=2.06 user=0.31s sys=1.04s rss=9624KB
run8_scp wall=0:07.75 sec=7.75 gbps=1.11 user=0.95s sys=1.94s rss=10632KB
[bench_quic_wan 22:02:52] === run9 (9/12) ===
run9_embrquic wall=0:09.36 sec=9.36 gbps=0.92 user=5.76s sys=3.41s rss=11996KB
run9_embrquicsm wall=0:09.55 sec=9.55 gbps=0.90 user=5.54s sys=3.82s rss=11996KB
run9_embrtcp wall=0:04.56 sec=4.56 gbps=1.88 user=2.85s sys=0.49s rss=9568KB
run9_nc wall=0:03.64 sec=3.64 gbps=2.36 user=0.27s sys=0.89s rss=9764KB
run9_scp wall=0:07.02 sec=7.02 gbps=1.22 user=1.03s sys=1.94s rss=12016KB
[bench_quic_wan 22:03:46] === run10 (10/12) ===
run10_embrquic wall=0:09.40 sec=9.4 gbps=0.91 user=5.57s sys=3.67s rss=11916KB
run10_embrquicsm wall=0:09.54 sec=9.54 gbps=0.90 user=5.80s sys=3.58s rss=12008KB
run10_embrtcp wall=0:04.75 sec=4.75 gbps=1.81 user=2.50s sys=0.85s rss=9632KB
run10_nc wall=0:03.82 sec=3.82 gbps=2.25 user=0.31s sys=0.84s rss=9708KB
run10_scp wall=0:06.59 sec=6.59 gbps=1.30 user=1.02s sys=1.92s rss=11672KB
[bench_quic_wan 22:04:39] === run11 (11/12) ===
run11_embrquic wall=0:09.46 sec=9.46 gbps=0.91 user=6.04s sys=3.27s rss=12064KB
run11_embrquicsm wall=0:09.67 sec=9.67 gbps=0.89 user=5.89s sys=3.59s rss=12004KB
run11_embrtcp wall=0:04.55 sec=4.55 gbps=1.89 user=2.74s sys=0.59s rss=9472KB
run11_nc wall=0:03.70 sec=3.7 gbps=2.32 user=0.31s sys=1.00s rss=9812KB
run11_scp wall=0:06.61 sec=6.61 gbps=1.30 user=1.10s sys=1.82s rss=11120KB
[bench_quic_wan 22:05:33] === run12 (12/12) ===
run12_embrquic wall=0:09.64 sec=9.64 gbps=0.89 user=5.58s sys=3.88s rss=11932KB
run12_embrquicsm wall=0:09.74 sec=9.74 gbps=0.88 user=5.63s sys=3.93s rss=12048KB
run12_embrtcp wall=0:03.92 sec=3.92 gbps=2.19 user=2.62s sys=0.72s rss=9528KB
run12_nc wall=0:03.14 sec=3.14 gbps=2.74 user=0.27s sys=1.13s rss=9880KB
run12_scp wall=0:07.89 sec=7.89 gbps=1.09 user=0.98s sys=1.98s rss=11028KB
[bench_quic_wan 22:06:27] === raw results ===
# bench_quic_wan Sun Sep  6 21:55:36 UTC 2026
# sender=44.198.164.219 cache=drop bytes=1073741824 build=./build
# kernel local=6.18.44-99.149.amzn2023.x86_64 remote=6.18.44-99.149.amzn2023.x86_64
# order: embrquic->embrquicsm->embrtcp->nc->scp
# embrquic:   ngtcp2+wolfSSL, mmap Buffer + in-place AEAD, io_uring SEND_ZC egress (registered slab)
# embrquicsm: same stack, EMBR_QUIC_EGRESS=sendmsg on the sender (per-datagram copy baseline)
# embrtcp:    sendfile/splice, plaintext.  nc: plaintext floor.  scp: encrypted reference

warmup1_embrquic wall=0:09.64 sec=9.64 gbps=0.89 user=5.83s sys=3.64s rss=11964KB
warmup1_embrquicsm wall=0:09.60 sec=9.6 gbps=0.89 user=5.51s sys=3.93s rss=12040KB
warmup1_embrtcp wall=0:04.35 sec=4.35 gbps=1.97 user=2.60s sys=0.70s rss=9536KB
warmup1_nc wall=0:04.28 sec=4.28 gbps=2.01 user=0.26s sys=1.05s rss=9668KB
warmup1_scp wall=0:07.75 sec=7.75 gbps=1.11 user=0.93s sys=2.03s rss=11216KB

warmup2_embrquic wall=0:09.42 sec=9.42 gbps=0.91 user=5.97s sys=3.29s rss=11896KB
warmup2_embrquicsm wall=0:09.51 sec=9.51 gbps=0.90 user=5.71s sys=3.64s rss=12032KB
warmup2_embrtcp wall=0:04.15 sec=4.15 gbps=2.07 user=2.62s sys=0.72s rss=9528KB
warmup2_nc wall=0:04.31 sec=4.31 gbps=1.99 user=0.28s sys=1.07s rss=9720KB
warmup2_scp wall=0:08.07 sec=8.07 gbps=1.06 user=1.07s sys=1.85s rss=11188KB

run3_embrquic wall=0:09.36 sec=9.36 gbps=0.92 user=5.67s sys=3.52s rss=11912KB
run3_embrquicsm wall=0:09.41 sec=9.41 gbps=0.91 user=5.58s sys=3.65s rss=11984KB
run3_embrtcp wall=0:04.13 sec=4.13 gbps=2.08 user=2.66s sys=0.71s rss=9564KB
run3_nc wall=0:04.29 sec=4.29 gbps=2.00 user=0.29s sys=1.12s rss=9740KB
run3_scp wall=0:07.70 sec=7.7 gbps=1.12 user=0.96s sys=2.03s rss=11076KB

run4_embrquic wall=0:09.41 sec=9.41 gbps=0.91 user=5.44s sys=3.78s rss=12064KB
run4_embrquicsm wall=0:09.47 sec=9.47 gbps=0.91 user=6.03s sys=3.29s rss=11980KB
run4_embrtcp wall=0:04.01 sec=4.01 gbps=2.14 user=2.54s sys=0.79s rss=9560KB
run4_nc wall=0:04.14 sec=4.14 gbps=2.07 user=0.15s sys=1.24s rss=9760KB
run4_scp wall=0:06.73 sec=6.73 gbps=1.28 user=0.85s sys=1.87s rss=10880KB

run5_embrquic wall=0:09.65 sec=9.65 gbps=0.89 user=5.71s sys=3.77s rss=11916KB
run5_embrquicsm wall=0:09.39 sec=9.39 gbps=0.91 user=6.20s sys=3.00s rss=12008KB
run5_embrtcp wall=0:04.08 sec=4.08 gbps=2.11 user=2.46s sys=0.89s rss=9536KB
run5_nc wall=0:03.58 sec=3.58 gbps=2.40 user=0.21s sys=1.19s rss=9648KB
run5_scp wall=0:07.93 sec=7.93 gbps=1.08 user=0.92s sys=1.99s rss=10712KB

run6_embrquic wall=0:09.29 sec=9.29 gbps=0.92 user=6.07s sys=3.05s rss=11948KB
run6_embrquicsm wall=0:09.35 sec=9.35 gbps=0.92 user=5.54s sys=3.66s rss=12032KB
run6_embrtcp wall=0:04.66 sec=4.66 gbps=1.84 user=2.67s sys=0.64s rss=9636KB
run6_nc wall=0:03.91 sec=3.91 gbps=2.20 user=0.19s sys=1.07s rss=9548KB
run6_scp wall=0:08.27 sec=8.27 gbps=1.04 user=0.89s sys=2.07s rss=10788KB

run7_embrquic wall=0:09.25 sec=9.25 gbps=0.93 user=6.18s sys=2.92s rss=12052KB
run7_embrquicsm wall=0:09.43 sec=9.43 gbps=0.91 user=5.93s sys=3.33s rss=12064KB
run7_embrtcp wall=0:04.45 sec=4.45 gbps=1.93 user=2.76s sys=0.54s rss=9560KB
run7_nc wall=0:04.12 sec=4.12 gbps=2.08 user=0.26s sys=1.05s rss=9796KB
run7_scp wall=0:07.75 sec=7.75 gbps=1.11 user=0.95s sys=1.86s rss=11092KB

run8_embrquic wall=0:09.44 sec=9.44 gbps=0.91 user=5.96s sys=3.32s rss=11896KB
run8_embrquicsm wall=0:09.56 sec=9.56 gbps=0.90 user=5.82s sys=3.55s rss=11912KB
run8_embrtcp wall=0:04.10 sec=4.1 gbps=2.10 user=2.61s sys=0.75s rss=9552KB
run8_nc wall=0:04.18 sec=4.18 gbps=2.06 user=0.31s sys=1.04s rss=9624KB
run8_scp wall=0:07.75 sec=7.75 gbps=1.11 user=0.95s sys=1.94s rss=10632KB

run9_embrquic wall=0:09.36 sec=9.36 gbps=0.92 user=5.76s sys=3.41s rss=11996KB
run9_embrquicsm wall=0:09.55 sec=9.55 gbps=0.90 user=5.54s sys=3.82s rss=11996KB
run9_embrtcp wall=0:04.56 sec=4.56 gbps=1.88 user=2.85s sys=0.49s rss=9568KB
run9_nc wall=0:03.64 sec=3.64 gbps=2.36 user=0.27s sys=0.89s rss=9764KB
run9_scp wall=0:07.02 sec=7.02 gbps=1.22 user=1.03s sys=1.94s rss=12016KB

run10_embrquic wall=0:09.40 sec=9.4 gbps=0.91 user=5.57s sys=3.67s rss=11916KB
run10_embrquicsm wall=0:09.54 sec=9.54 gbps=0.90 user=5.80s sys=3.58s rss=12008KB
run10_embrtcp wall=0:04.75 sec=4.75 gbps=1.81 user=2.50s sys=0.85s rss=9632KB
run10_nc wall=0:03.82 sec=3.82 gbps=2.25 user=0.31s sys=0.84s rss=9708KB
run10_scp wall=0:06.59 sec=6.59 gbps=1.30 user=1.02s sys=1.92s rss=11672KB

run11_embrquic wall=0:09.46 sec=9.46 gbps=0.91 user=6.04s sys=3.27s rss=12064KB
run11_embrquicsm wall=0:09.67 sec=9.67 gbps=0.89 user=5.89s sys=3.59s rss=12004KB
run11_embrtcp wall=0:04.55 sec=4.55 gbps=1.89 user=2.74s sys=0.59s rss=9472KB
run11_nc wall=0:03.70 sec=3.7 gbps=2.32 user=0.31s sys=1.00s rss=9812KB
run11_scp wall=0:06.61 sec=6.61 gbps=1.30 user=1.10s sys=1.82s rss=11120KB

run12_embrquic wall=0:09.64 sec=9.64 gbps=0.89 user=5.58s sys=3.88s rss=11932KB
run12_embrquicsm wall=0:09.74 sec=9.74 gbps=0.88 user=5.63s sys=3.93s rss=12048KB
run12_embrtcp wall=0:03.92 sec=3.92 gbps=2.19 user=2.62s sys=0.72s rss=9528KB
run12_nc wall=0:03.14 sec=3.14 gbps=2.74 user=0.27s sys=1.13s rss=9880KB
run12_scp wall=0:07.89 sec=7.89 gbps=1.09 user=0.98s sys=1.98s rss=11028KB

[bench_quic_wan 22:06:27] === summary (warmup excluded) — median [min..max] ===

tool       | throughput Gbps    | wall sec           | user s   | sys s   
-----------+--------------------+--------------------+----------+---------
embrquic   | 0.9100  [0.89 0.93] | 9.4050  [9.25 9.65] | 5.7350   | 3.4650  
embrquicsm | 0.9050  [0.88 0.92] | 9.5050  [9.35 9.74] | 5.8100   | 3.5850  
embrtcp    | 2.0050  [1.81 2.19] | 4.2900  [3.92 4.75] | 2.6400   | 0.7150  
nc         | 2.2250  [2.00 2.74] | 3.8650  [3.14 4.29] | 0.2700   | 1.0600  
scp        | 1.1150  [1.04 1.30] | 7.7250  [6.59 8.27] | 0.9550   | 1.9400  
[bench_quic_wan 22:06:27] raw results in /tmp/bench_quic_wan_results.txt
[bench_quic_wan 22:06:27] read: embrquic vs embrquicsm = SEND_ZC vs sendmsg, receiver-side wall; the sender's sys s
[bench_quic_wan 22:06:27]       is the copy-floor number (run `time` around the push loop there if you need it);
[bench_quic_wan 22:06:27]       embrquic vs embrtcp Gbps = transport cost at the path ceiling;
[bench_quic_wan 22:06:27]       embrquic vs scp = encrypted-vs-encrypted; sys s = kernel work per GiB
````
<!-- raw-end:bench_quic_wan_receiver.terminal.txt -->

### bench_quic_wan_sender.embrquic

Original bytes: 266; SHA-256: dd67e2091d42c3403f51ca35b434b24f15fdfd0a96eb59ca98aa89651586aacc.

<!-- raw-begin:bench_quic_wan_sender.embrquic -->
````text
27.16 7.43 3.03 0
54.74 3.14 3.06 0
54.80 3.02 2.76 0
54.50 2.79 2.57 0
53.52 2.64 3.28 0
53.73 3.06 3.08 0
54.93 2.81 2.52 0
54.65 2.63 2.95 0
54.49 3.02 3.03 0
53.61 2.74 2.99 0
53.78 2.72 3.34 0
53.67 2.65 2.91 0
Command terminated by signal 2
120.21 0.00 0.00 0
````
<!-- raw-end:bench_quic_wan_sender.embrquic -->

### bench_quic_wan_sender.embrquicsm

Original bytes: 266; SHA-256: a0ab920e9e264b5bfce5c55019bb5d742f4be308ea36d2900b2f7f4013b2c9bb.

<!-- raw-begin:bench_quic_wan_sender.embrquicsm -->
````text
40.21 7.93 2.36 0
54.65 3.14 2.61 0
54.73 2.92 2.28 0
54.56 3.16 2.76 0
53.46 2.88 2.56 0
53.65 3.16 2.50 0
55.07 3.29 2.56 0
54.78 2.93 2.48 0
54.47 2.86 2.66 0
53.59 3.58 2.53 0
53.89 2.86 2.75 0
53.75 2.95 2.57 0
Command terminated by signal 2
106.99 0.00 0.00 0
````
<!-- raw-end:bench_quic_wan_sender.embrquicsm -->

### bench_quic_wan_sender.embrtcp

Original bytes: 321; SHA-256: c3555cbb682cc36bd29cf2a121b290d716cb57373d322aab8b85cc6db77cb7dc.

<!-- raw-begin:bench_quic_wan_sender.embrtcp -->
````text
Command exited with non-zero status 1
11.33 4.56 0.06 1
36.35 0.00 0.19 0
54.49 0.01 0.20 0
54.79 0.00 0.32 0
54.39 0.01 0.35 0
53.46 0.00 0.36 0
54.28 0.01 0.31 0
54.87 0.01 0.30 0
54.42 0.00 0.33 0
54.89 0.00 0.31 0
53.82 0.00 0.27 0
53.68 0.00 0.32 0
53.08 0.00 0.39 0
Command terminated by signal 2
99.63 0.00 0.00 0
````
<!-- raw-end:bench_quic_wan_sender.embrtcp -->

