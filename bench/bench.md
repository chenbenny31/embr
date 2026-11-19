# Benchmark measurements and raw data

The transfer campaign below was reported against the v0.8 tree (`9342d1e`,
Release build) on 2026-09-05/06. Transfer timing ends without an explicit fsync;
it measures transfer completion, not durable storage. Section 1 contains older
v0.7 syscall counts. The [corrected desktop ownership benchmark](#desktop-ownership-cost)
is a separate run after the benchmark implementation was repaired. The
[packet-loss runs](#packet-loss-runs) in §8, the
[GSO loopback runs](#gso-loopback) in §9 and the
[send-cost runs](#egress-cost) in §10 are from November 2026.

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
datagram:            1350 bytes (QUIC_MAX_PKTLEN) where the path carries them, else 1200.
                     ngtcp2 starts at QUIC's 1200-byte minimum and sends one 1350-byte path-MTU
                     probe; if the probe is lost the size stays 1200. Sockets set DF
                     (IP_PMTUDISC_PROBE) and a datagram the path refuses (EMSGSIZE, or a
                     router's ICMP) counts as lost. Until 2026-11-15 the size never rose:
                     ngtcp2's default first probe (1406 bytes) did not fit the buffer, so
                     every earlier run in this file, September's WAN included, sent 1200
egress slab:         without GSO, QUIC_ZC_SLOTS = 64 slots × 1350 B per connection, registered
                     once; about 105 KiB locked per connection including the ring.
                     With GSO, QUIC_ZC_GSO_SLOTS = 8 slots × 16 × 1350 B (169 KiB slab).
                     A slot is busy until the NIC frees its packets; with none free the
                     sender waits for a completion. EMBR_QUIC_ZC_SLOTS=N (1-1024) sets the
                     count for experiments (§10). The slab counts against the user's locked
                     memory (8 MiB by default, shared by all of the user's processes): 128
                     GSO slots are 2.8 MiB, so a third concurrent connection no longer
                     registers and runs on sendmsg
GSO (after v0.8 WAN): same-size packets are laid end to end, up to QUIC_GSO_SEGS = 16, and
                     leave as one send; the socket's UDP_SEGMENT is set to the packet size.
                     A batch ends when it is full, when a packet of another size arrives,
                     or before the sender waits. EMBR_QUIC_GSO=0 sends one packet per send
completion rule:     send CQE with F_MORE → hold the slot until NOTIF; without F_MORE → final
fallbacks:           refused datagram (ENOMEM/ENOBUFS/EAGAIN/EINVAL/EOPNOTSUPP) copy-sent once;
                     no registration or kernel < 6.2 → sendmsg for the connection;
                     slab still held at teardown → leaked, never reused
ACK threshold:       2 packets (ngtcp2's default). EMBR_QUIC_ACK_THRESH=N (1-100) makes an
                     endpoint acknowledge every N packets; it matters on the receiver (§9)
knobs:               EMBR_QUIC_EGRESS=sendmsg (copying baseline), EMBR_QUIC_GSO=0,
                     EMBR_QUIC_STATS=1 (per-connection mode/gso/datagrams/sends/bytes, and for
                     SEND_ZC notifs/copied/fallback/errors/waits, at close; waits counts the
                     times the sender blocked for a free slot)
flow control:        4 MiB per stream, 8 MiB per connection, one bidi stream
receive loop:        select + one recvmsg per datagram (v0.8); recvmmsg batching is v0.9
certificate:         the token carries a hash of the sender's certificate and pull checks it
                     after the handshake; pulling by bare address skips the check
pacing:              off (ngtcp2_conn_update_pkt_tx_time is never called); sends are ACK-clocked
```

---

## §6 — Reproduction

Run these commands from the repository root. Use a Release binary in `./build`
for each harness; the directory name alone does not establish the build type.

**Dependencies and Release build (both WAN hosts, or the loopback host):**

```bash
BUILD_EMBR=1 EMBR_BUILD_DIR=./build bash install.sh
```

The setup script installs dependencies, updates UDP socket limits, checks
kernel/memlock prerequisites and builds embr. With dependencies already prepared,
use the [README Release build commands](../README.md#build).

All harnesses are modes of one script, `bench/bench.sh`. The September campaign
ran its predecessors (`quic_wan.sh`, `tcp_loopback.sh`, `quic_loopback.sh`), which
that script merges; the raw blocks in §11 keep their original command lines.
`ROWS="..."` picks and orders the rows.

**WAN, all seven rows** (the five of §2 plus the two GSO rows):

```bash
# sender (us-east-1) — open inbound TCP 22, 10007, 9999 and UDP 10008-10011
FILE_SIZE_GB=1 bash bench/bench.sh wan-sender

# receiver (us-east-2)
SENDER_IP="<sender-ip>" SSH_KEY=~/.ssh/key.pem bash bench/bench.sh wan-receiver
```

**Loopback:**

```bash
ROWS="embrtcp nc scp" bash bench/bench.sh loopback          # §3: cache=drop
CACHE_DISCIPLINE=keep bash bench/bench.sh loopback          # §4 and §9: embr TCP, the four QUIC rows, nc
```

The four QUIC rows are `embrquic` (SEND_ZC) and `embrquicsm` (sendmsg), both with one
packet per send as in September, and `embrquicgso` / `embrquicsmgso` with GSO batching.

**§9's ACK-threshold rows** set `EMBR_QUIC_ACK_THRESH` for the whole run; on two hosts
set it on the receiver. The results header records the value.

```bash
EMBR_QUIC_ACK_THRESH=10 CACHE_DISCIPLINE=keep RUNS=10 \
    ROWS="embrquic embrquicsm embrquicgso embrquicsmgso" bash bench/bench.sh loopback
```

Since 2026-11-15 each result line also carries `host_cpu` / `host_irq`, and on the WAN
receiver `sender_cpu` / `sender_irq`: whole-machine CPU seconds over the transfer. They
include packet work the kernel does in softirq, which no process's user/sys shows
(Fedora's kernel keeps softirq time out of process time). The runs archived in this
file predate these fields.

**The `SEND_ZC` slot pool** is a sender setting: start the sender with
`EMBR_QUIC_ZC_SLOTS=128` to run the `embrquic` rows with a larger pool. The sender
summary's `waits` total says how often the sender blocked for a free slot. §10's
send-only tables come from `bench/egress_cost.sh`, which needs no second host and no root.

QUIC rows use the certificate `install.sh` creates in `~/.config/embr/`
(`CERT=` / `KEY=` override). The loopback scp row requires a working localhost
SSH login. A row's environment is set on the sender only; the September loopback
run also set `EMBR_QUIC_EGRESS=sendmsg` on the receiver for the sendmsg row.

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
Release and ASan/UBSan preflight runs passed.

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

<a id="packet-loss-runs"></a>
## §8 — Packet-loss runs (desktop, network namespace)

Run on 2026-11-14/15 on the Fedora desktop (kernel 7.3.3), on the tree that became
`3783d38`. Separate from the September campaign; nothing here changes §2–§4.

### Setup

- One private network namespace per run (`unshare --user --map-root-user --net`),
  traffic over `lo`. No root, no change to the host's network.
- Loss on the nftables **input** hook: `numgen random mod 100 < N drop`, one rule
  matching the listen port as destination and one as source, so N% is dropped in
  each direction. Dropping on the output hook is not loss: the send call itself
  fails with `EPERM`.
- Workload: `embr push` / `embr pull --transport quic` of a 20 MB random file,
  checked with `cmp`; 8 transfers at a time.
- "Stall" means both ends sat silent until the 30 s idle timeout
  (`ERR_IDLE_CLOSE` on both sides, 30.4 s after the last progress).

### What the runs found

| problem | loss | before | after |
|---|---|---|---|
| lost first packet | 10% | 6 of 24 connections failed | 0 of 36 |
| "connection refused" on send | none | 11 failures in 2,400 | 0 in 4,800 |
| sends queued, not submitted | 10% | 13.2–16.6 s per transfer | 1.4–2.3 s |
| stall at the minimum window | 10% | 2 stalls in about 640 transfers | 0 in 1,504 |

**Lost first packet.** The TLS hello carries a post-quantum key share and no
longer fits one packet, so a client opens with two. When the first was lost,
ngtcp2 told the server to drop the half-made connection; embr treated that as
fatal and the push process exited (`quic_accept: first packet rejected`).
`quic_accept` now discards any packet a connection cannot start from and keeps
listening; the client's retransmission one second later connects.

**"Connection refused" on send.** After a peer closes its socket the kernel
reports ICMP port-unreachable on the next socket call. embr tolerated that on
receive and failed on send, so the result depended on which call came first.
Measured without loss, as 8 concurrent × 300 repeats of a client that connects
and leaves. Both paths now count it as a lost packet.

**Sends queued, not submitted.** With the io_uring sender a packet is only queued
until the ring is submitted. The send loop went to wait for ACKs with packets
still queued, so each round moved only when a retransmit timer fired. `sendmsg`
on the same runs: 1.4–1.9 s. Without loss the change is not measurable
(1 GiB: 2.64 s io_uring, 2.39 s `sendmsg`, in line with §4).

**Stall at the minimum window.** With the window at its two-packet minimum and
both packets retransmission probes, losing both left ngtcp2 up to 1.25, acting
as server, with no probe to send and its timer cancelled while still counting
them in flight. ngtcp2 1.26.0 sends a PING probe instead. "Before" is the old
sender on ngtcp2 1.22.0; "after" is the same old sender on 1.26.0, so the
upgrade alone removes the stall. With the sender fixed, 1.22.0 completed
600 of 600 (io_uring) and 480 of 480 (`sendmsg`) at 10% loss: the queued-send
delay is what made the two-probe case likely.

### 30% loss and the handshake limit

Fixed sender, 96 transfers per row (12 batches of 8), 30% dropped each way:

| ngtcp2 | complete | stalls | handshake timeouts | seconds per transfer, min / median / max |
|---|---|---|---|---|
| 1.22.0 | 93 of 96 | 2 | 1 | 104.8 / 128.4 / 161.2 |
| 1.26.0 | 94 of 96 | 0 | 2 | 105.8 / 130.8 / 195.3 |

Every completed transfer matched the source file.

The handshake limit (`QUIC_HANDSHAKE_TIMEOUT_NS`) is 10 s. At 30% loss, 3 of 192
handshakes (1.6%) did not finish inside it and pull exited after 10.0 s with
`no answer from 127.0.0.1:PORT within 10 s`. The handshake retransmit timer
starts near 1 s and doubles, so about four attempts fit in the limit. None of
the 2,584 transfers at 10% loss hit it.

**Decision (2026-11-15): the limit stays at 10 s.** 30% loss is far outside
normal use, the failure is prompt and says what happened, and a retry is one
command; a longer limit would make "nothing is listening" slower to report for
everyone else.

Failed runs (port, exit code, seconds, ngtcp2 expiry result, error):

```
ngtcp2 1.22.0
FAIL 14011 rc=1 145.6  ERR_IDLE_CLOSE (pull and push)   [error] QuicTransport::recv_file: pump failed
FAIL 14045 rc=1  10.0  ERR_HANDSHAKE_TIMEOUT            [error] quic_connect: no answer from 127.0.0.1:14045 within 10 s
FAIL 14082 rc=1 121.1  ERR_IDLE_CLOSE (pull and push)   [error] QuicTransport::recv_file: pump failed

ngtcp2 1.26.0
FAIL 14082 rc=1  10.0  [error] quic_connect: no answer from 127.0.0.1:14082 within 10 s
FAIL 14114 rc=1  10.0  [error] quic_connect: no answer from 127.0.0.1:14114 within 10 s
```

### Reproduce one transfer

```bash
head -c 20000000 /dev/urandom > f.bin
LOSS=30   # percent dropped in each direction
unshare --user --map-root-user --net bash -c "
  ip link set lo up
  nft add table inet t
  nft add chain inet t in '{ type filter hook input priority 0; }'
  nft add rule inet t in udp dport 14001 numgen random mod 100 \< $LOSS drop
  nft add rule inet t in udp sport 14001 numgen random mod 100 \< $LOSS drop
  ./build/embr push f.bin --transport quic --port 14001 &
  sleep 0.5
  ./build/embr pull 127.0.0.1 --port 14001 --transport quic --out got.bin
  wait; cmp f.bin got.bin && echo SAME"
```

`EMBR_QUIC_LOG=1` on either side prints the ngtcp2 trace used above.

---

<a id="gso-loopback"></a>
## §9 — UDP GSO × egress × ACK threshold (loopback, 1 GiB)

Run on 2026-11-15 on the Fedora desktop (kernel 7.3.3, ngtcp2 1.26.0, Release build,
cache kept, as §4), with `bench/bench.sh loopback`. This is the loopback half of the
2×2 the September WAN result asked for: `SEND_ZC` against `sendmsg`, each with one
packet per send and with GSO batching. The real-NIC half (two hosts) is not run yet.

Loopback copies every `SEND_ZC` send (`copied` = 100% in every row below), so nothing
here measures zero-copy. It measures what a send costs and how GSO spreads that cost.

### Median summary (n = 10, warmup excluded)

"ACK threshold" is the receiver's `ack_thresh`: it acknowledges after that many
packets. 2 is ngtcp2's default and what embr ships. "Packets per send" is the sender's
datagrams ÷ sends from its `[quic-egress]` lines. Sender sys shows median [min max].

| ACK threshold | sender egress | packets per send | throughput | wall | send_user | send_sys | recv_user | recv_sys |
|---|---|---|---|---|---|---|---|---|
| 2 | SEND_ZC | 1.0 | 3.33 Gbps | 2.58 s | 0.69 s | 1.04 s [1.02 1.09] | 1.09 s | 1.07 s |
| 2 | sendmsg | 1.0 | 3.41 Gbps | 2.52 s | 0.70 s | 0.95 s [0.92 1.19] | 1.02 s | 1.08 s |
| 2 | SEND_ZC + GSO | 2.0 | 3.40 Gbps | 2.53 s | 0.67 s | 0.81 s [0.79 0.87] | 1.01 s | 1.08 s |
| 2 | sendmsg + GSO | 2.0 | 3.45 Gbps | 2.50 s | 0.68 s | 0.75 s [0.72 0.96] | 1.02 s | 1.06 s |
| 10 | SEND_ZC | 1.0 | 4.46 Gbps | 1.92 s | 0.38 s | 0.76 s [0.75 0.77] | 1.00 s | 0.68 s |
| 10 | sendmsg | 1.0 | 4.70 Gbps | 1.83 s | 0.43 s | 0.74 s [0.73 0.77] | 1.00 s | 0.68 s |
| 10 | SEND_ZC + GSO | 9.7 | 4.81 Gbps | 1.79 s | 0.48 s | 0.34 s [0.32 0.36] | 1.01 s | 0.66 s |
| 10 | sendmsg + GSO | 9.8 | 4.84 Gbps | 1.77 s | 0.52 s | 0.33 s [0.30 0.37] | 1.00 s | 0.66 s |
| 32 | SEND_ZC | 1.0 | 4.61 Gbps | 1.87 s | 0.34 s | 0.74 s [0.72 0.75] | 1.01 s | 0.61 s |
| 32 | sendmsg | 1.0 | 4.98 Gbps | 1.73 s | 0.39 s | 0.70 s [0.67 0.73] | 1.02 s | 0.57 s |
| 32 | SEND_ZC + GSO | 14.9 | 5.24 Gbps | 1.64 s | 0.41 s | 0.26 s [0.24 0.29] | 1.02 s | 0.58 s |
| 32 | sendmsg + GSO | 14.9 | 5.29 Gbps | 1.62 s | 0.45 s | 0.26 s [0.24 0.28] | 1.00 s | 0.57 s |

Same run, for scale (the ACK threshold does not apply):

| row | throughput | wall | send_user | send_sys | recv_user | recv_sys |
|---|---|---|---|---|---|---|
| embr TCP | 13.85 Gbps | 0.62 s | 0.00 s | 0.02 s | 0.39 s | 0.21 s |
| nc | 31.24 Gbps | 0.28 s | 0.02 s | 0.16 s | 0.02 s | 0.22 s |

### How to read this

**GSO needs something to batch.** The sender has no pacer; it sends what each
acknowledgement frees. At the default threshold an ACK arrives every 2 packets and
frees 2, so a GSO batch is 2 packets, never the 16 the code allows. Even so, sender
kernel time falls 22% with `SEND_ZC` (1.04 → 0.81 s) and 21% with `sendmsg`
(0.95 → 0.75 s).

**Fewer ACKs help with or without GSO.** At threshold 10 the receiver sends a fifth of
the ACKs: its kernel time falls from 1.07 to 0.68 s, the sender's user time from 0.69
to 0.38 s, and throughput rises from 3.33 to 4.46 Gbps before GSO does anything.

**With real batches GSO halves the sender's kernel time.** At threshold 10 batches are
9.7 packets and sender kernel time is 0.34 s against 0.76 s (`SEND_ZC`) and 0.33 s
against 0.74 s (`sendmsg`); at threshold 32, 14.9 packets and 0.26 s against 0.74 s
and 0.70 s. Throughput at threshold 32 with GSO is 5.2–5.3 Gbps against 3.3–3.5 Gbps
at the default threshold. Sender user time rises by 0.06–0.10 s with GSO at
thresholds 10 and 32; the cause was not investigated.

**`SEND_ZC` against `sendmsg`.** Without GSO `SEND_ZC` costs 0.02–0.09 s more sender
kernel time than `sendmsg`, as in §4: on loopback it pays the copy and the
notification. With batches of 10 or more the two are level (0.34 / 0.33 s, 0.26 /
0.26 s): one notification now covers a whole batch. Whether `SEND_ZC` then comes out
ahead when the NIC really reads user pages is the question for the two-host run;
[§10](#egress-cost) measures the send alone, without the loopback copy.

**Limits.** Loopback only. Thresholds 10 and 32 were set with `EMBR_QUIC_ACK_THRESH`
(`settings.ack_thresh`) on both ends, on a build that had the switch before the tree
did; the tree has it now and the default stays 2. A high threshold delays ACKs
whenever the congestion window is smaller than the threshold, so it is not a free
default.

### Under 10% loss

Setup of §8: 20 MB transfers, 10% dropped each way, 8 at a time. Median seconds per
transfer; every transfer in every cell completed and matched the source.

| ACK threshold | SEND_ZC | sendmsg | SEND_ZC + GSO | sendmsg + GSO | transfers per cell |
|---|---|---|---|---|---|
| 2 (shipped) | 1.7 s | 1.8 s | 1.5 s | 1.6 s | 48 |
| 10 | 2.6 s | 2.3 s | 2.7 s | 2.1 s | 24 |
| 32 | 2.5 s | 2.4 s | 2.3 s | 2.3 s | 24 |

GSO does not hurt under loss. A higher ACK threshold does: transfers take roughly
30–80% longer. The likely cause, not traced, is that after a loss the window is smaller
than the threshold, so the receiver waits out its ACK delay.

When packets are lost the batches also meet mixed sizes (a 1194-byte packet followed by
1200-byte ones, an ACK followed by data): 266 such cases in 48 transfers on an
instrumented build, none in a loss-free 1 GiB transfer. The sanitizer build ran 32 of
these lossy transfers with no report.

### Packet size

Every table above was measured with 1200-byte data packets: ngtcp2 starts there, and its
default first path-MTU probe (1406 bytes) did not fit embr's 1350-byte buffer, so the size
never rose. Since 2026-11-15 embr names the probe size: one 1350-byte probe after the
handshake, 1350-byte packets once it is acknowledged, 1200 if it is lost.

Same loopback setup, ACK threshold 2, n = 10, medians. The 1350 rows are this tree with the
probe; its stats lines report 1343 bytes per datagram and 830,000 datagrams per GiB
instead of 937,000 (−11%). `host_irq` is whole-machine irq + softirq time per transfer
(§6), not recorded for the earlier rows.

| packet size | sender egress | throughput | send_user | send_sys | recv_user | recv_sys | host_irq |
|---|---|---|---|---|---|---|---|
| 1200 | SEND_ZC | 3.33 Gbps | 0.69 s | 1.04 s | 1.09 s | 1.07 s | — |
| 1200 | sendmsg | 3.41 Gbps | 0.70 s | 0.95 s | 1.02 s | 1.08 s | — |
| 1200 | SEND_ZC + GSO | 3.40 Gbps | 0.67 s | 0.81 s | 1.01 s | 1.08 s | — |
| 1200 | sendmsg + GSO | 3.45 Gbps | 0.68 s | 0.75 s | 1.02 s | 1.06 s | — |
| 1350 | SEND_ZC | 3.67 Gbps | 0.61 s | 0.93 s | 1.01 s | 0.96 s | 1.17 s |
| 1350 | sendmsg | 3.79 Gbps | 0.60 s | 0.87 s | 0.93 s | 0.96 s | 1.00 s |
| 1350 | SEND_ZC + GSO | 3.80 Gbps | 0.58 s | 0.72 s | 0.92 s | 0.97 s | 0.99 s |
| 1350 | sendmsg + GSO | 3.80 Gbps | 0.59 s | 0.67 s | 0.94 s | 0.95 s | 0.89 s |

Throughput rises 10–12% and CPU falls on both ends. A run of the same code earlier the
same day gave 7–9%, so read it as about 10%.

**Narrow paths.** 1350 bytes of UDP payload need an IPv4 MTU of 1378. Raising the size
alone broke narrower paths in testing: a GSO send failed with "Message too long", a
non-GSO send was silently fragmented by the kernel, and a router's ICMP "fragmentation
needed" surfaced as a fatal receive error. So the sockets set DF and never fragment
(`IP_PMTUDISC_PROBE`), and a send or receive that reports `EMSGSIZE` counts as a lost
packet. 64 MiB transfers in network namespaces, all four egress × GSO modes each, every
file intact:

| where the small MTU is | MTU at or above 1378 | MTU 1377, 1300, 1280 |
|---|---|---|
| the sender's own interface | 1350-byte packets | 1200-byte packets |
| a router on the path that answers with ICMP | 1350-byte packets | 1200-byte packets |
| a router on the path, ICMP blocked (MTU 1300 only) | not run | 1200-byte packets |

**Limit.** A path whose MTU drops below 1378 after the packets have grown is not
detected; ngtcp2 1.26 has no handling for it that we could find. Tested by lowering a
router hop from 1500 to 1300 one second into a 1 GiB transfer: the pull stalled and failed
at the 30 s idle timeout with 476 of 1024 chunks, and a second pull resumed from them and
finished with 1200-byte packets.

### Raw results

ACK threshold 2:

```
# bench loopback Sun Nov 15 11:47:45 AM PST 2026
# commit=3783d38-dirty build=/tmp/claude-1000/-home-benny-Code-embr/9f9b0fff-9827-476a-8878-ae1838deff48/scratchpad/build-126-Release cache=keep bytes=1073741824
# rows: embrtcp embrquic embrquicsm embrquicgso embrquicsmgso nc
# kernel=7.3.3-300.fc45.x86_64 memlock=8192KiB — loopback copies every SEND_ZC; compare CPU seconds, not wall
# embrtcp: sendfile/splice, plaintext.  embrquic: mmap Buffer + in-place AEAD + io_uring SEND_ZC egress.
# embrquicsm: same with EMBR_QUIC_EGRESS=sendmsg on the sender.  *gso: UDP GSO on (EMBR_QUIC_GSO=1), else off.
# nc: plaintext floor.  scp: encrypted reference.
run2_embrtcp wall=0:00.62 sec=0.62 gbps=13.85 send_user=0.00s send_sys=0.02s recv_user=0.39s recv_sys=0.20s
run2_embrquic wall=0:02.59 sec=2.59 gbps=3.32 send_user=0.70s send_sys=1.03s recv_user=1.11s recv_sys=1.04s
run2_embrquicsm wall=0:02.50 sec=2.5 gbps=3.44 send_user=0.69s send_sys=0.92s recv_user=0.99s recv_sys=1.08s
run2_embrquicgso wall=0:02.55 sec=2.55 gbps=3.37 send_user=0.67s send_sys=0.80s recv_user=1.01s recv_sys=1.10s
run2_embrquicsmgso wall=0:02.51 sec=2.51 gbps=3.42 send_user=0.67s send_sys=0.76s recv_user=1.04s recv_sys=1.04s
run2_nc wall=0:00.27 sec=0.27 gbps=31.81 send_user=0.02s send_sys=0.17s recv_user=0.02s recv_sys=0.22s
run3_embrtcp wall=0:00.61 sec=0.61 gbps=14.08 send_user=0.00s send_sys=0.02s recv_user=0.39s recv_sys=0.20s
run3_embrquic wall=0:02.64 sec=2.64 gbps=3.25 send_user=0.72s send_sys=1.04s recv_user=1.09s recv_sys=1.10s
run3_embrquicsm wall=0:02.54 sec=2.54 gbps=3.38 send_user=0.72s send_sys=0.98s recv_user=0.97s recv_sys=1.13s
run3_embrquicgso wall=0:02.52 sec=2.52 gbps=3.41 send_user=0.65s send_sys=0.82s recv_user=1.01s recv_sys=1.09s
run3_embrquicsmgso wall=0:02.49 sec=2.49 gbps=3.45 send_user=0.65s send_sys=0.75s recv_user=0.99s recv_sys=1.08s
run3_nc wall=0:00.26 sec=0.26 gbps=33.04 send_user=0.02s send_sys=0.16s recv_user=0.02s recv_sys=0.22s
run4_embrtcp wall=0:00.62 sec=0.62 gbps=13.85 send_user=0.00s send_sys=0.02s recv_user=0.38s recv_sys=0.21s
run4_embrquic wall=0:02.57 sec=2.57 gbps=3.34 send_user=0.68s send_sys=1.04s recv_user=1.05s recv_sys=1.09s
run4_embrquicsm wall=0:02.57 sec=2.57 gbps=3.34 send_user=0.74s send_sys=0.94s recv_user=1.09s recv_sys=1.05s
run4_embrquicgso wall=0:02.55 sec=2.55 gbps=3.37 send_user=0.68s send_sys=0.81s recv_user=1.03s recv_sys=1.09s
run4_embrquicsmgso wall=0:02.77 sec=2.77 gbps=3.10 send_user=0.83s send_sys=0.87s recv_user=1.04s recv_sys=1.27s
run4_nc wall=0:00.29 sec=0.29 gbps=29.62 send_user=0.01s send_sys=0.16s recv_user=0.02s recv_sys=0.22s
run5_embrtcp wall=0:00.62 sec=0.62 gbps=13.85 send_user=0.00s send_sys=0.03s recv_user=0.39s recv_sys=0.21s
run5_embrquic wall=0:02.58 sec=2.58 gbps=3.33 send_user=0.71s send_sys=1.05s recv_user=1.10s recv_sys=1.04s
run5_embrquicsm wall=0:02.48 sec=2.48 gbps=3.46 send_user=0.68s send_sys=0.95s recv_user=1.03s recv_sys=1.03s
run5_embrquicgso wall=0:02.54 sec=2.54 gbps=3.38 send_user=0.70s send_sys=0.79s recv_user=1.05s recv_sys=1.06s
run5_embrquicsmgso wall=0:02.46 sec=2.46 gbps=3.49 send_user=0.67s send_sys=0.72s recv_user=0.98s recv_sys=1.05s
run5_nc wall=0:00.28 sec=0.28 gbps=30.68 send_user=0.01s send_sys=0.15s recv_user=0.02s recv_sys=0.21s
run6_embrtcp wall=0:00.61 sec=0.61 gbps=14.08 send_user=0.00s send_sys=0.02s recv_user=0.39s recv_sys=0.19s
run6_embrquic wall=0:02.61 sec=2.61 gbps=3.29 send_user=0.68s send_sys=1.09s recv_user=1.09s recv_sys=1.08s
run6_embrquicsm wall=0:02.48 sec=2.48 gbps=3.46 send_user=0.70s send_sys=0.92s recv_user=1.00s recv_sys=1.05s
run6_embrquicgso wall=0:02.51 sec=2.51 gbps=3.42 send_user=0.66s send_sys=0.82s recv_user=1.01s recv_sys=1.07s
run6_embrquicsmgso wall=0:02.47 sec=2.47 gbps=3.48 send_user=0.69s send_sys=0.72s recv_user=0.97s recv_sys=1.08s
run6_nc wall=0:00.27 sec=0.27 gbps=31.81 send_user=0.02s send_sys=0.16s recv_user=0.03s recv_sys=0.21s
run7_embrtcp wall=0:00.62 sec=0.62 gbps=13.85 send_user=0.00s send_sys=0.02s recv_user=0.39s recv_sys=0.20s
run7_embrquic wall=0:02.55 sec=2.55 gbps=3.37 send_user=0.69s send_sys=1.02s recv_user=1.09s recv_sys=1.03s
run7_embrquicsm wall=0:02.47 sec=2.47 gbps=3.48 send_user=0.66s send_sys=0.95s recv_user=0.99s recv_sys=1.04s
run7_embrquicgso wall=0:02.55 sec=2.55 gbps=3.37 send_user=0.68s send_sys=0.82s recv_user=1.00s recv_sys=1.11s
run7_embrquicsmgso wall=0:02.52 sec=2.52 gbps=3.41 send_user=0.66s send_sys=0.77s recv_user=1.04s recv_sys=1.04s
run7_nc wall=0:00.28 sec=0.28 gbps=30.68 send_user=0.02s send_sys=0.15s recv_user=0.02s recv_sys=0.21s
run8_embrtcp wall=0:00.61 sec=0.61 gbps=14.08 send_user=0.00s send_sys=0.02s recv_user=0.38s recv_sys=0.20s
run8_embrquic wall=0:02.62 sec=2.62 gbps=3.28 send_user=0.71s send_sys=1.05s recv_user=1.11s recv_sys=1.07s
run8_embrquicsm wall=0:02.48 sec=2.48 gbps=3.46 send_user=0.70s send_sys=0.92s recv_user=0.97s recv_sys=1.08s
run8_embrquicgso wall=0:02.52 sec=2.52 gbps=3.41 send_user=0.65s send_sys=0.80s recv_user=1.05s recv_sys=1.05s
run8_embrquicsmgso wall=0:02.49 sec=2.49 gbps=3.45 send_user=0.70s send_sys=0.72s recv_user=1.04s recv_sys=1.02s
run8_nc wall=0:00.27 sec=0.27 gbps=31.81 send_user=0.01s send_sys=0.17s recv_user=0.02s recv_sys=0.22s
run9_embrtcp wall=0:00.63 sec=0.63 gbps=13.63 send_user=0.00s send_sys=0.02s recv_user=0.39s recv_sys=0.21s
run9_embrquic wall=0:02.55 sec=2.55 gbps=3.37 send_user=0.68s send_sys=1.02s recv_user=1.05s recv_sys=1.07s
run9_embrquicsm wall=0:02.59 sec=2.59 gbps=3.32 send_user=0.74s send_sys=0.96s recv_user=1.05s recv_sys=1.10s
run9_embrquicgso wall=0:02.51 sec=2.51 gbps=3.42 send_user=0.67s send_sys=0.79s recv_user=1.01s recv_sys=1.07s
run9_embrquicsmgso wall=0:02.50 sec=2.5 gbps=3.44 send_user=0.71s send_sys=0.73s recv_user=1.01s recv_sys=1.07s
run9_nc wall=0:00.28 sec=0.28 gbps=30.68 send_user=0.02s send_sys=0.17s recv_user=0.02s recv_sys=0.23s
run10_embrtcp wall=0:00.62 sec=0.62 gbps=13.85 send_user=0.00s send_sys=0.02s recv_user=0.38s recv_sys=0.21s
run10_embrquic wall=0:02.58 sec=2.58 gbps=3.33 send_user=0.71s send_sys=1.02s recv_user=1.11s recv_sys=1.04s
run10_embrquicsm wall=0:02.55 sec=2.55 gbps=3.37 send_user=0.70s send_sys=0.97s recv_user=1.06s recv_sys=1.07s
run10_embrquicgso wall=0:02.70 sec=2.7 gbps=3.18 send_user=0.78s send_sys=0.87s recv_user=1.05s recv_sys=1.19s
run10_embrquicsmgso wall=0:02.49 sec=2.49 gbps=3.45 send_user=0.67s send_sys=0.75s recv_user=1.01s recv_sys=1.05s
run10_nc wall=0:00.28 sec=0.28 gbps=30.68 send_user=0.02s send_sys=0.14s recv_user=0.03s recv_sys=0.20s
run11_embrtcp wall=0:00.62 sec=0.62 gbps=13.85 send_user=0.00s send_sys=0.02s recv_user=0.38s recv_sys=0.21s
run11_embrquic wall=0:02.59 sec=2.59 gbps=3.32 send_user=0.69s send_sys=1.05s recv_user=1.07s recv_sys=1.09s
run11_embrquicsm wall=0:02.93 sec=2.93 gbps=2.93 send_user=0.79s send_sys=1.19s recv_user=1.13s recv_sys=1.28s
run11_embrquicgso wall=0:02.51 sec=2.51 gbps=3.42 send_user=0.67s send_sys=0.81s recv_user=1.01s recv_sys=1.07s
run11_embrquicsmgso wall=0:02.95 sec=2.95 gbps=2.91 send_user=0.91s send_sys=0.96s recv_user=1.12s recv_sys=1.34s
run11_nc wall=0:00.26 sec=0.26 gbps=33.04 send_user=0.02s send_sys=0.15s recv_user=0.02s recv_sys=0.22s

embrquic egress over 11 connections: datagrams=10311443 sends=10311443 notifs=10311443 copied=10311443 fallback=0 errors=0 (1.0 datagrams per send, copied 100.0%)
embrquicsm egress over 11 connections: datagrams=10297835 sends=10297835 (1.0 datagrams per send)
embrquicgso egress over 11 connections: datagrams=10301836 sends=5085606 notifs=5085606 copied=5085606 fallback=0 errors=0 (2.0 datagrams per send, copied 100.0%)
embrquicsmgso egress over 11 connections: datagrams=10300382 sends=5065807 (2.0 datagrams per send)
```

ACK threshold 10:

```
# bench loopback Sun Nov 15 11:54:16 AM PST 2026
# commit=3783d38-dirty build=/tmp/claude-1000/-home-benny-Code-embr/9f9b0fff-9827-476a-8878-ae1838deff48/scratchpad/gso-tree/build-Release cache=keep bytes=1073741824
# rows: embrquic embrquicsm embrquicgso embrquicsmgso   ack_thresh=10
# kernel=7.3.3-300.fc45.x86_64 memlock=8192KiB — loopback copies every SEND_ZC; compare CPU seconds, not wall
# embrtcp: sendfile/splice, plaintext.  embrquic: mmap Buffer + in-place AEAD + io_uring SEND_ZC egress.
# embrquicsm: same with EMBR_QUIC_EGRESS=sendmsg on the sender.  *gso: UDP GSO on (EMBR_QUIC_GSO=1), else off.
# nc: plaintext floor.  scp: encrypted reference.
run2_embrquic wall=0:01.94 sec=1.94 gbps=4.43 send_user=0.39s send_sys=0.76s recv_user=0.98s recv_sys=0.68s
run2_embrquicsm wall=0:01.81 sec=1.81 gbps=4.75 send_user=0.43s send_sys=0.73s recv_user=1.01s recv_sys=0.68s
run2_embrquicgso wall=0:01.79 sec=1.79 gbps=4.80 send_user=0.51s send_sys=0.32s recv_user=1.01s recv_sys=0.66s
run2_embrquicsmgso wall=0:01.77 sec=1.77 gbps=4.85 send_user=0.51s send_sys=0.33s recv_user=0.96s recv_sys=0.69s
run3_embrquic wall=0:01.91 sec=1.91 gbps=4.50 send_user=0.37s send_sys=0.75s recv_user=1.00s recv_sys=0.65s
run3_embrquicsm wall=0:01.84 sec=1.84 gbps=4.67 send_user=0.46s send_sys=0.73s recv_user=1.00s recv_sys=0.70s
run3_embrquicgso wall=0:01.78 sec=1.78 gbps=4.83 send_user=0.48s send_sys=0.34s recv_user=1.03s recv_sys=0.64s
run3_embrquicsmgso wall=0:01.77 sec=1.77 gbps=4.85 send_user=0.51s send_sys=0.33s recv_user=1.04s recv_sys=0.62s
run4_embrquic wall=0:01.93 sec=1.93 gbps=4.45 send_user=0.38s send_sys=0.76s recv_user=0.98s recv_sys=0.70s
run4_embrquicsm wall=0:01.79 sec=1.79 gbps=4.80 send_user=0.41s send_sys=0.75s recv_user=0.97s recv_sys=0.69s
run4_embrquicgso wall=0:01.80 sec=1.8 gbps=4.77 send_user=0.50s send_sys=0.34s recv_user=1.01s recv_sys=0.67s
run4_embrquicsmgso wall=0:01.78 sec=1.78 gbps=4.83 send_user=0.51s send_sys=0.33s recv_user=1.00s recv_sys=0.66s
run5_embrquic wall=0:01.95 sec=1.95 gbps=4.41 send_user=0.38s send_sys=0.77s recv_user=0.99s recv_sys=0.70s
run5_embrquicsm wall=0:01.84 sec=1.84 gbps=4.67 send_user=0.45s send_sys=0.74s recv_user=1.06s recv_sys=0.65s
run5_embrquicgso wall=0:01.79 sec=1.79 gbps=4.80 send_user=0.48s send_sys=0.34s recv_user=0.97s recv_sys=0.70s
run5_embrquicsmgso wall=0:01.81 sec=1.81 gbps=4.75 send_user=0.52s send_sys=0.37s recv_user=1.03s recv_sys=0.66s
run6_embrquic wall=0:01.95 sec=1.95 gbps=4.41 send_user=0.39s send_sys=0.77s recv_user=1.02s recv_sys=0.68s
run6_embrquicsm wall=0:01.85 sec=1.85 gbps=4.64 send_user=0.44s send_sys=0.77s recv_user=1.00s recv_sys=0.71s
run6_embrquicgso wall=0:01.78 sec=1.78 gbps=4.83 send_user=0.49s send_sys=0.34s recv_user=0.95s recv_sys=0.71s
run6_embrquicsmgso wall=0:01.78 sec=1.78 gbps=4.83 send_user=0.51s send_sys=0.34s recv_user=1.01s recv_sys=0.66s
run7_embrquic wall=0:01.92 sec=1.92 gbps=4.47 send_user=0.38s send_sys=0.76s recv_user=1.00s recv_sys=0.68s
run7_embrquicsm wall=0:01.79 sec=1.79 gbps=4.80 send_user=0.42s send_sys=0.73s recv_user=0.98s recv_sys=0.68s
run7_embrquicgso wall=0:01.78 sec=1.78 gbps=4.83 send_user=0.48s send_sys=0.34s recv_user=1.01s recv_sys=0.66s
run7_embrquicsmgso wall=0:01.77 sec=1.77 gbps=4.85 send_user=0.54s send_sys=0.31s recv_user=0.99s recv_sys=0.66s
run8_embrquic wall=0:01.93 sec=1.93 gbps=4.45 send_user=0.37s send_sys=0.77s recv_user=1.02s recv_sys=0.65s
run8_embrquicsm wall=0:01.85 sec=1.85 gbps=4.64 send_user=0.46s send_sys=0.74s recv_user=0.97s recv_sys=0.73s
run8_embrquicgso wall=0:01.78 sec=1.78 gbps=4.83 send_user=0.49s send_sys=0.35s recv_user=1.07s recv_sys=0.61s
run8_embrquicsmgso wall=0:01.77 sec=1.77 gbps=4.85 send_user=0.53s send_sys=0.32s recv_user=1.02s recv_sys=0.64s
run9_embrquic wall=0:01.92 sec=1.92 gbps=4.47 send_user=0.38s send_sys=0.76s recv_user=0.99s recv_sys=0.67s
run9_embrquicsm wall=0:01.82 sec=1.82 gbps=4.72 send_user=0.42s send_sys=0.74s recv_user=1.04s recv_sys=0.64s
run9_embrquicgso wall=0:01.79 sec=1.79 gbps=4.80 send_user=0.47s send_sys=0.36s recv_user=1.02s recv_sys=0.66s
run9_embrquicsmgso wall=0:01.76 sec=1.76 gbps=4.88 send_user=0.52s send_sys=0.32s recv_user=0.96s recv_sys=0.69s
run10_embrquic wall=0:01.92 sec=1.92 gbps=4.47 send_user=0.37s send_sys=0.77s recv_user=1.01s recv_sys=0.66s
run10_embrquicsm wall=0:01.81 sec=1.81 gbps=4.75 send_user=0.41s send_sys=0.75s recv_user=1.01s recv_sys=0.66s
run10_embrquicgso wall=0:01.78 sec=1.78 gbps=4.83 send_user=0.49s send_sys=0.34s recv_user=1.01s recv_sys=0.66s
run10_embrquicsmgso wall=0:01.79 sec=1.79 gbps=4.80 send_user=0.54s send_sys=0.30s recv_user=1.02s recv_sys=0.65s
run11_embrquic wall=0:01.91 sec=1.91 gbps=4.50 send_user=0.39s send_sys=0.75s recv_user=1.01s recv_sys=0.67s
run11_embrquicsm wall=0:01.84 sec=1.84 gbps=4.67 send_user=0.44s send_sys=0.75s recv_user=1.04s recv_sys=0.66s
run11_embrquicgso wall=0:01.80 sec=1.8 gbps=4.77 send_user=0.48s send_sys=0.35s recv_user=1.03s recv_sys=0.66s
run11_embrquicsmgso wall=0:01.80 sec=1.8 gbps=4.77 send_user=0.51s send_sys=0.35s recv_user=0.99s recv_sys=0.69s

embrquic egress over 11 connections: datagrams=10286117 sends=10286117 notifs=10286117 copied=10286117 fallback=0 errors=0 (1.0 datagrams per send, copied 100.0%)
embrquicsm egress over 11 connections: datagrams=10284357 sends=10284357 (1.0 datagrams per send)
embrquicgso egress over 11 connections: datagrams=10294304 sends=1060232 notifs=1060232 copied=1060232 fallback=0 errors=0 (9.7 datagrams per send, copied 100.0%)
embrquicsmgso egress over 11 connections: datagrams=10292903 sends=1055203 (9.8 datagrams per send)
```

ACK threshold 32:

```
# bench loopback Sun Nov 15 11:56:21 AM PST 2026
# commit=3783d38-dirty build=/tmp/claude-1000/-home-benny-Code-embr/9f9b0fff-9827-476a-8878-ae1838deff48/scratchpad/gso-tree/build-Release cache=keep bytes=1073741824
# rows: embrquic embrquicsm embrquicgso embrquicsmgso   ack_thresh=32
# kernel=7.3.3-300.fc45.x86_64 memlock=8192KiB — loopback copies every SEND_ZC; compare CPU seconds, not wall
# embrtcp: sendfile/splice, plaintext.  embrquic: mmap Buffer + in-place AEAD + io_uring SEND_ZC egress.
# embrquicsm: same with EMBR_QUIC_EGRESS=sendmsg on the sender.  *gso: UDP GSO on (EMBR_QUIC_GSO=1), else off.
# nc: plaintext floor.  scp: encrypted reference.
run2_embrquic wall=0:01.89 sec=1.89 gbps=4.54 send_user=0.35s send_sys=0.74s recv_user=1.05s recv_sys=0.59s
run2_embrquicsm wall=0:01.73 sec=1.73 gbps=4.97 send_user=0.37s send_sys=0.72s recv_user=0.98s recv_sys=0.62s
run2_embrquicgso wall=0:01.66 sec=1.66 gbps=5.17 send_user=0.40s send_sys=0.29s recv_user=1.02s recv_sys=0.58s
run2_embrquicsmgso wall=0:01.63 sec=1.63 gbps=5.27 send_user=0.45s send_sys=0.27s recv_user=0.98s recv_sys=0.60s
run3_embrquic wall=0:01.84 sec=1.84 gbps=4.67 send_user=0.32s send_sys=0.74s recv_user=0.98s recv_sys=0.62s
run3_embrquicsm wall=0:01.73 sec=1.73 gbps=4.97 send_user=0.39s send_sys=0.69s recv_user=1.05s recv_sys=0.56s
run3_embrquicgso wall=0:01.66 sec=1.66 gbps=5.17 send_user=0.42s send_sys=0.26s recv_user=1.04s recv_sys=0.58s
run3_embrquicsmgso wall=0:01.66 sec=1.66 gbps=5.17 send_user=0.47s send_sys=0.25s recv_user=1.02s recv_sys=0.59s
run4_embrquic wall=0:01.93 sec=1.93 gbps=4.45 send_user=0.37s send_sys=0.75s recv_user=1.03s recv_sys=0.65s
run4_embrquicsm wall=0:01.72 sec=1.72 gbps=4.99 send_user=0.41s send_sys=0.68s recv_user=1.03s recv_sys=0.57s
run4_embrquicgso wall=0:01.64 sec=1.64 gbps=5.24 send_user=0.41s send_sys=0.29s recv_user=1.02s recv_sys=0.57s
run4_embrquicsmgso wall=0:01.65 sec=1.65 gbps=5.21 send_user=0.44s send_sys=0.27s recv_user=1.05s recv_sys=0.55s
run5_embrquic wall=0:01.87 sec=1.87 gbps=4.59 send_user=0.36s send_sys=0.72s recv_user=1.01s recv_sys=0.61s
run5_embrquicsm wall=0:01.71 sec=1.71 gbps=5.02 send_user=0.39s send_sys=0.68s recv_user=1.01s recv_sys=0.57s
run5_embrquicgso wall=0:01.64 sec=1.64 gbps=5.24 send_user=0.41s send_sys=0.26s recv_user=1.04s recv_sys=0.55s
run5_embrquicsmgso wall=0:01.61 sec=1.61 gbps=5.34 send_user=0.44s send_sys=0.27s recv_user=0.99s recv_sys=0.57s
run6_embrquic wall=0:01.85 sec=1.85 gbps=4.64 send_user=0.33s send_sys=0.73s recv_user=1.00s recv_sys=0.61s
run6_embrquicsm wall=0:01.73 sec=1.73 gbps=4.97 send_user=0.35s send_sys=0.73s recv_user=1.01s recv_sys=0.58s
run6_embrquicgso wall=0:01.64 sec=1.64 gbps=5.24 send_user=0.41s send_sys=0.27s recv_user=1.00s recv_sys=0.59s
run6_embrquicsmgso wall=0:01.62 sec=1.62 gbps=5.30 send_user=0.43s send_sys=0.27s recv_user=1.03s recv_sys=0.54s
run7_embrquic wall=0:01.92 sec=1.92 gbps=4.47 send_user=0.36s send_sys=0.74s recv_user=1.00s recv_sys=0.66s
run7_embrquicsm wall=0:01.73 sec=1.73 gbps=4.97 send_user=0.40s send_sys=0.68s recv_user=1.04s recv_sys=0.55s
run7_embrquicgso wall=0:01.64 sec=1.64 gbps=5.24 send_user=0.44s send_sys=0.24s recv_user=1.00s recv_sys=0.59s
run7_embrquicsmgso wall=0:01.62 sec=1.62 gbps=5.30 send_user=0.46s send_sys=0.24s recv_user=1.00s recv_sys=0.57s
run8_embrquic wall=0:01.86 sec=1.86 gbps=4.62 send_user=0.33s send_sys=0.74s recv_user=1.02s recv_sys=0.59s
run8_embrquicsm wall=0:01.70 sec=1.7 gbps=5.05 send_user=0.40s send_sys=0.67s recv_user=1.03s recv_sys=0.56s
run8_embrquicgso wall=0:01.64 sec=1.64 gbps=5.24 send_user=0.41s send_sys=0.26s recv_user=1.01s recv_sys=0.57s
run8_embrquicsmgso wall=0:01.62 sec=1.62 gbps=5.30 send_user=0.44s send_sys=0.24s recv_user=1.00s recv_sys=0.56s
run9_embrquic wall=0:01.88 sec=1.88 gbps=4.57 send_user=0.34s send_sys=0.74s recv_user=1.01s recv_sys=0.62s
run9_embrquicsm wall=0:01.72 sec=1.72 gbps=4.99 send_user=0.38s send_sys=0.71s recv_user=0.97s recv_sys=0.62s
run9_embrquicgso wall=0:01.64 sec=1.64 gbps=5.24 send_user=0.41s send_sys=0.26s recv_user=1.03s recv_sys=0.56s
run9_embrquicsmgso wall=0:01.61 sec=1.61 gbps=5.34 send_user=0.45s send_sys=0.25s recv_user=1.01s recv_sys=0.55s
run10_embrquic wall=0:01.86 sec=1.86 gbps=4.62 send_user=0.32s send_sys=0.75s recv_user=0.98s recv_sys=0.63s
run10_embrquicsm wall=0:01.74 sec=1.74 gbps=4.94 send_user=0.37s send_sys=0.72s recv_user=1.01s recv_sys=0.60s
run10_embrquicgso wall=0:01.64 sec=1.64 gbps=5.24 send_user=0.40s send_sys=0.27s recv_user=1.01s recv_sys=0.58s
run10_embrquicsmgso wall=0:01.63 sec=1.63 gbps=5.27 send_user=0.45s send_sys=0.25s recv_user=1.00s recv_sys=0.58s
run11_embrquic wall=0:01.85 sec=1.85 gbps=4.64 send_user=0.31s send_sys=0.75s recv_user=1.01s recv_sys=0.60s
run11_embrquicsm wall=0:01.71 sec=1.71 gbps=5.02 send_user=0.35s send_sys=0.72s recv_user=1.03s recv_sys=0.56s
run11_embrquicgso wall=0:01.64 sec=1.64 gbps=5.24 send_user=0.42s send_sys=0.26s recv_user=0.99s recv_sys=0.60s
run11_embrquicsmgso wall=0:01.64 sec=1.64 gbps=5.24 send_user=0.42s send_sys=0.28s recv_user=1.01s recv_sys=0.58s

embrquic egress over 11 connections: datagrams=10286635 sends=10286635 notifs=10286635 copied=10286635 fallback=0 errors=0 (1.0 datagrams per send, copied 100.0%)
embrquicsm egress over 11 connections: datagrams=10285031 sends=10285031 (1.0 datagrams per send)
embrquicgso egress over 11 connections: datagrams=10292375 sends=690561 notifs=690561 copied=690561 fallback=0 errors=0 (14.9 datagrams per send, copied 100.0%)
embrquicsmgso egress over 11 connections: datagrams=10292362 sends=690055 (14.9 datagrams per send)
```

1350-byte packets, ACK threshold 2 (the tree with the path-MTU probe):

```
# bench loopback Sun Nov 15 07:56:32 PM PST 2026
# commit=3783d38-dirty build=/tmp/claude-1000/-home-benny-Code-embr/9f9b0fff-9827-476a-8878-ae1838deff48/scratchpad/build-126-Release cache=keep bytes=1073741824
# rows: embrtcp embrquic embrquicsm embrquicgso embrquicsmgso nc   ack_thresh=2
# kernel=7.3.3-300.fc45.x86_64 memlock=8192KiB — loopback copies every SEND_ZC; compare CPU seconds, not wall
# embrtcp: sendfile/splice, plaintext.  embrquic: mmap Buffer + in-place AEAD + io_uring SEND_ZC egress.
# embrquicsm: same with EMBR_QUIC_EGRESS=sendmsg on the sender.  *gso: UDP GSO on (EMBR_QUIC_GSO=1), else off.
# nc: plaintext floor.  scp: encrypted reference.

warmup1_embrtcp wall=0:00.67 sec=0.67 gbps=12.82 send_user=0.00s send_sys=0.03s recv_user=0.39s recv_sys=0.25s host_cpu=1.10s host_irq=0.07s
warmup1_embrquic wall=0:02.40 sec=2.4 gbps=3.58 send_user=0.64s send_sys=0.96s recv_user=1.06s recv_sys=0.93s host_cpu=6.91s host_irq=1.28s
warmup1_embrquicsm wall=0:02.28 sec=2.28 gbps=3.77 send_user=0.63s send_sys=0.87s recv_user=0.91s recv_sys=0.99s host_cpu=6.14s host_irq=1.07s
warmup1_embrquicgso wall=0:02.28 sec=2.28 gbps=3.77 send_user=0.59s send_sys=0.74s recv_user=0.92s recv_sys=0.98s host_cpu=5.46s host_irq=1.05s
warmup1_embrquicsmgso wall=0:02.23 sec=2.23 gbps=3.85 send_user=0.58s send_sys=0.68s recv_user=0.90s recv_sys=0.95s host_cpu=4.61s host_irq=0.92s
warmup1_nc wall=0:00.26 sec=0.26 gbps=33.04 send_user=0.02s send_sys=0.18s recv_user=0.02s recv_sys=0.21s host_cpu=0.65s host_irq=0.06s

run2_embrtcp wall=0:00.66 sec=0.66 gbps=13.02 send_user=0.00s send_sys=0.03s recv_user=0.39s recv_sys=0.25s host_cpu=0.97s host_irq=0.05s
run2_embrquic wall=0:02.35 sec=2.35 gbps=3.66 send_user=0.63s send_sys=0.92s recv_user=1.01s recv_sys=0.95s host_cpu=5.13s host_irq=1.21s
run2_embrquicsm wall=0:02.26 sec=2.26 gbps=3.80 send_user=0.64s send_sys=0.84s recv_user=0.89s recv_sys=0.99s host_cpu=5.02s host_irq=1.02s
run2_embrquicgso wall=0:02.27 sec=2.27 gbps=3.78 send_user=0.58s send_sys=0.73s recv_user=0.92s recv_sys=0.97s host_cpu=4.66s host_irq=1.02s
run2_embrquicsmgso wall=0:02.24 sec=2.24 gbps=3.83 send_user=0.61s send_sys=0.66s recv_user=0.94s recv_sys=0.92s host_cpu=4.62s host_irq=0.90s
run2_nc wall=0:00.27 sec=0.27 gbps=31.81 send_user=0.02s send_sys=0.18s recv_user=0.01s recv_sys=0.23s host_cpu=0.62s host_irq=0.05s

run3_embrtcp wall=0:00.67 sec=0.67 gbps=12.82 send_user=0.00s send_sys=0.04s recv_user=0.39s recv_sys=0.25s host_cpu=0.95s host_irq=0.06s
run3_embrquic wall=0:02.34 sec=2.34 gbps=3.67 send_user=0.62s send_sys=0.93s recv_user=1.01s recv_sys=0.95s host_cpu=5.05s host_irq=1.18s
run3_embrquicsm wall=0:02.27 sec=2.27 gbps=3.78 send_user=0.60s send_sys=0.86s recv_user=0.93s recv_sys=0.96s host_cpu=4.87s host_irq=1.01s
run3_embrquicgso wall=0:02.27 sec=2.27 gbps=3.78 send_user=0.55s send_sys=0.76s recv_user=0.94s recv_sys=0.95s host_cpu=4.69s host_irq=1.01s
run3_embrquicsmgso wall=0:02.26 sec=2.26 gbps=3.80 send_user=0.61s send_sys=0.66s recv_user=0.91s recv_sys=0.97s host_cpu=4.59s host_irq=0.91s
run3_nc wall=0:00.27 sec=0.27 gbps=31.81 send_user=0.02s send_sys=0.18s recv_user=0.03s recv_sys=0.21s host_cpu=0.65s host_irq=0.05s

run4_embrtcp wall=0:00.66 sec=0.66 gbps=13.02 send_user=0.00s send_sys=0.04s recv_user=0.39s recv_sys=0.24s host_cpu=1.09s host_irq=0.06s
run4_embrquic wall=0:02.32 sec=2.32 gbps=3.70 send_user=0.63s send_sys=0.92s recv_user=0.94s recv_sys=1.00s host_cpu=4.99s host_irq=1.18s
run4_embrquicsm wall=0:02.26 sec=2.26 gbps=3.80 send_user=0.56s send_sys=0.89s recv_user=0.89s recv_sys=0.99s host_cpu=4.64s host_irq=0.98s
run4_embrquicgso wall=0:02.26 sec=2.26 gbps=3.80 send_user=0.58s send_sys=0.72s recv_user=0.93s recv_sys=0.95s host_cpu=4.59s host_irq=0.99s
run4_embrquicsmgso wall=0:02.25 sec=2.25 gbps=3.82 send_user=0.58s send_sys=0.67s recv_user=0.91s recv_sys=0.97s host_cpu=4.40s host_irq=0.89s
run4_nc wall=0:00.26 sec=0.26 gbps=33.04 send_user=0.02s send_sys=0.17s recv_user=0.01s recv_sys=0.22s host_cpu=0.56s host_irq=0.04s

run5_embrtcp wall=0:00.64 sec=0.64 gbps=13.42 send_user=0.00s send_sys=0.03s recv_user=0.38s recv_sys=0.24s host_cpu=1.26s host_irq=0.05s
run5_embrquic wall=0:02.31 sec=2.31 gbps=3.72 send_user=0.60s send_sys=0.92s recv_user=0.95s recv_sys=0.99s host_cpu=4.89s host_irq=1.17s
run5_embrquicsm wall=0:02.27 sec=2.27 gbps=3.78 send_user=0.63s send_sys=0.87s recv_user=0.92s recv_sys=0.96s host_cpu=5.19s host_irq=1.01s
run5_embrquicgso wall=0:02.30 sec=2.3 gbps=3.73 send_user=0.62s send_sys=0.69s recv_user=0.95s recv_sys=0.98s host_cpu=4.62s host_irq=0.94s
run5_embrquicsmgso wall=0:02.24 sec=2.24 gbps=3.83 send_user=0.59s send_sys=0.66s recv_user=0.95s recv_sys=0.91s host_cpu=4.78s host_irq=0.90s
run5_nc wall=0:00.27 sec=0.27 gbps=31.81 send_user=0.01s send_sys=0.18s recv_user=0.02s recv_sys=0.22s host_cpu=0.56s host_irq=0.04s

run6_embrtcp wall=0:00.65 sec=0.65 gbps=13.22 send_user=0.00s send_sys=0.03s recv_user=0.37s recv_sys=0.25s host_cpu=1.14s host_irq=0.05s
run6_embrquic wall=0:02.39 sec=2.39 gbps=3.59 send_user=0.61s send_sys=0.96s recv_user=1.03s recv_sys=0.98s host_cpu=5.04s host_irq=1.18s
run6_embrquicsm wall=0:02.24 sec=2.24 gbps=3.83 send_user=0.58s send_sys=0.87s recv_user=0.93s recv_sys=0.93s host_cpu=4.61s host_irq=0.98s
run6_embrquicgso wall=0:02.26 sec=2.26 gbps=3.80 send_user=0.58s send_sys=0.71s recv_user=0.90s recv_sys=0.99s host_cpu=4.43s host_irq=0.98s
run6_embrquicsmgso wall=0:02.25 sec=2.25 gbps=3.82 send_user=0.59s send_sys=0.67s recv_user=0.94s recv_sys=0.94s host_cpu=4.24s host_irq=0.87s
run6_nc wall=0:00.26 sec=0.26 gbps=33.04 send_user=0.01s send_sys=0.18s recv_user=0.02s recv_sys=0.22s host_cpu=0.57s host_irq=0.04s

run7_embrtcp wall=0:00.66 sec=0.66 gbps=13.02 send_user=0.00s send_sys=0.04s recv_user=0.38s recv_sys=0.26s host_cpu=0.86s host_irq=0.05s
run7_embrquic wall=0:02.33 sec=2.33 gbps=3.69 send_user=0.63s send_sys=0.91s recv_user=0.97s recv_sys=0.98s host_cpu=4.85s host_irq=1.15s
run7_embrquicsm wall=0:02.24 sec=2.24 gbps=3.83 send_user=0.62s send_sys=0.86s recv_user=0.90s recv_sys=0.96s host_cpu=4.60s host_irq=1.00s
run7_embrquicgso wall=0:02.27 sec=2.27 gbps=3.78 send_user=0.57s send_sys=0.72s recv_user=0.95s recv_sys=0.95s host_cpu=4.48s host_irq=0.96s
run7_embrquicsmgso wall=0:02.26 sec=2.26 gbps=3.80 send_user=0.59s send_sys=0.67s recv_user=0.95s recv_sys=0.94s host_cpu=4.31s host_irq=0.88s
run7_nc wall=0:00.27 sec=0.27 gbps=31.81 send_user=0.01s send_sys=0.18s recv_user=0.01s recv_sys=0.23s host_cpu=0.62s host_irq=0.04s

run8_embrtcp wall=0:00.66 sec=0.66 gbps=13.02 send_user=0.00s send_sys=0.04s recv_user=0.39s recv_sys=0.24s host_cpu=0.89s host_irq=0.04s
run8_embrquic wall=0:02.35 sec=2.35 gbps=3.66 send_user=0.61s send_sys=0.95s recv_user=0.98s recv_sys=0.99s host_cpu=5.03s host_irq=1.16s
run8_embrquicsm wall=0:02.25 sec=2.25 gbps=3.82 send_user=0.63s send_sys=0.84s recv_user=0.93s recv_sys=0.94s host_cpu=4.57s host_irq=0.99s
run8_embrquicgso wall=0:02.26 sec=2.26 gbps=3.80 send_user=0.58s send_sys=0.72s recv_user=0.92s recv_sys=0.97s host_cpu=4.52s host_irq=0.99s
run8_embrquicsmgso wall=0:02.26 sec=2.26 gbps=3.80 send_user=0.58s send_sys=0.67s recv_user=0.93s recv_sys=0.95s host_cpu=4.37s host_irq=0.89s
run8_nc wall=0:00.26 sec=0.26 gbps=33.04 send_user=0.01s send_sys=0.18s recv_user=0.01s recv_sys=0.22s host_cpu=0.55s host_irq=0.04s

run9_embrtcp wall=0:00.66 sec=0.66 gbps=13.02 send_user=0.00s send_sys=0.04s recv_user=0.40s recv_sys=0.24s host_cpu=0.81s host_irq=0.04s
run9_embrquic wall=0:02.38 sec=2.38 gbps=3.61 send_user=0.60s send_sys=0.98s recv_user=1.05s recv_sys=0.94s host_cpu=5.55s host_irq=1.18s
run9_embrquicsm wall=0:02.27 sec=2.27 gbps=3.78 send_user=0.60s send_sys=0.88s recv_user=0.95s recv_sys=0.93s host_cpu=4.81s host_irq=1.01s
run9_embrquicgso wall=0:02.26 sec=2.26 gbps=3.80 send_user=0.60s send_sys=0.70s recv_user=0.90s recv_sys=0.98s host_cpu=4.53s host_irq=1.00s
run9_embrquicsmgso wall=0:02.30 sec=2.3 gbps=3.73 send_user=0.63s send_sys=0.65s recv_user=0.96s recv_sys=0.95s host_cpu=4.43s host_irq=0.88s
run9_nc wall=0:00.27 sec=0.27 gbps=31.81 send_user=0.02s send_sys=0.16s recv_user=0.02s recv_sys=0.21s host_cpu=0.57s host_irq=0.05s

run10_embrtcp wall=0:00.66 sec=0.66 gbps=13.02 send_user=0.00s send_sys=0.04s recv_user=0.40s recv_sys=0.24s host_cpu=0.85s host_irq=0.04s
run10_embrquic wall=0:02.34 sec=2.34 gbps=3.67 send_user=0.60s send_sys=0.93s recv_user=1.03s recv_sys=0.93s host_cpu=4.95s host_irq=1.17s
run10_embrquicsm wall=0:02.30 sec=2.3 gbps=3.73 send_user=0.59s send_sys=0.91s recv_user=0.94s recv_sys=0.97s host_cpu=5.06s host_irq=1.01s
run10_embrquicgso wall=0:02.26 sec=2.26 gbps=3.80 send_user=0.62s send_sys=0.68s recv_user=0.91s recv_sys=0.97s host_cpu=4.70s host_irq=0.99s
run10_embrquicsmgso wall=0:02.28 sec=2.28 gbps=3.77 send_user=0.59s send_sys=0.68s recv_user=0.92s recv_sys=0.98s host_cpu=4.52s host_irq=0.89s
run10_nc wall=0:00.27 sec=0.27 gbps=31.81 send_user=0.02s send_sys=0.17s recv_user=0.02s recv_sys=0.22s host_cpu=0.57s host_irq=0.04s

run11_embrtcp wall=0:00.66 sec=0.66 gbps=13.02 send_user=0.00s send_sys=0.04s recv_user=0.40s recv_sys=0.24s host_cpu=1.03s host_irq=0.05s
run11_embrquic wall=0:02.36 sec=2.36 gbps=3.64 send_user=0.60s send_sys=0.96s recv_user=1.02s recv_sys=0.95s host_cpu=5.04s host_irq=1.17s
run11_embrquicsm wall=0:02.27 sec=2.27 gbps=3.78 send_user=0.60s send_sys=0.88s recv_user=0.94s recv_sys=0.96s host_cpu=4.80s host_irq=1.00s
run11_embrquicgso wall=0:02.25 sec=2.25 gbps=3.82 send_user=0.55s send_sys=0.74s recv_user=0.89s recv_sys=0.99s host_cpu=4.61s host_irq=0.96s
run11_embrquicsmgso wall=0:02.26 sec=2.26 gbps=3.80 send_user=0.60s send_sys=0.66s recv_user=0.91s recv_sys=0.98s host_cpu=4.46s host_irq=0.87s
run11_nc wall=0:00.26 sec=0.26 gbps=33.04 send_user=0.01s send_sys=0.16s recv_user=0.02s recv_sys=0.21s host_cpu=0.64s host_irq=0.05s

embrquic egress over 11 connections: datagrams=9128728 sends=9128728 bytes=12260855792 notifs=9128728 copied=9128728 fallback=0 errors=0 (1.0 datagrams per send, copied 100.0%, 1343 bytes per datagram)
embrquicsm egress over 11 connections: datagrams=9115651 sends=9115651 bytes=12240901658 (1.0 datagrams per send, 1343 bytes per datagram)
embrquicgso egress over 11 connections: datagrams=9119259 sends=4334547 bytes=12247375815 notifs=4334547 copied=4334547 fallback=0 errors=0 (2.1 datagrams per send, copied 100.0%, 1343 bytes per datagram)
embrquicsmgso egress over 11 connections: datagrams=9116173 sends=4342407 bytes=12242710809 (2.1 datagrams per send, 1343 bytes per datagram)
```

---

<a id="egress-cost"></a>
## §10 — What one send costs: `SEND_ZC` against `sendmsg` (send-only, desktop)

Run on 2026-11-15 on the Fedora desktop (kernel 7.3.3). `bench/egress_cost.cpp` drives the
tree's `ZcEgress` and a plain `sendmsg` loop in the call pattern `QuicTransport` uses, with
no QUIC and no receiver, on a dummy network device in a private namespace. A dummy device
frees every packet at transmit, so no `SEND_ZC` is copied (copied = 0 in every row below).
One pinned core, 1350-byte packets. Numbers are nanoseconds of CPU per send (user + sys +
softirq): median of 5 runs, or of 3 where the device delays.

### The device frees each packet at once

| packets per send | bytes | `sendmsg` | `SEND_ZC` | `SEND_ZC` − `sendmsg` |
|---|---|---|---|---|
| 1 | 1,350 | 342 | 425 | +83 |
| 2 | 2,700 | 375 | 527 | +152 |
| 4 | 5,400 | 404 | 547 | +143 |
| 8 | 10,800 | 450 | 586 | +136 |
| 10 | 13,500 | 472 | 609 | +137 |
| 12 | 16,200 | 507 | 637 | +130 |
| 14 | 18,900 | 546 | 641 | +95 |
| 16 | 21,600 | 611 | 680 | +69 |
| 20 | 27,000 | 725 | 724 | −1 |
| 24 | 32,400 | 834 | 769 | −65 |
| 32 | 43,200 | 1280 | 870 | −410 |
| 47 | 63,450 | 1562 | 1039 | −523 |

### The device holds each packet for 100 µs

A netem qdisc in front of the dummy device delays every packet, and with it the `SEND_ZC`
completion that frees the slot, as a NIC's transmit queue does. The slot count is
`ZcEgress`'s pool; embr uses 8 with GSO.

| packets per send | `sendmsg` | `SEND_ZC`, 8 slots | 16 slots | 128 slots |
|---|---|---|---|---|
| 2 | 590 | 1000 | 934 | 689 |
| 8 | 805 | 1061 | 1037 | 800 |
| 10 | 876 | 1109 | 1060 | 826 |
| 12 | 952 | 1096 | 994 | 824 |
| 14 | 1048 | 1181 | 1069 | 895 |
| 16 | 1116 | 1225 | 1116 | 901 |
| wall time per send | as CPU | 12.9 µs | 6.4 µs | 0.8–0.9 µs |

### In a transfer (loopback, 1 GiB, n = 5, GSO on)

| ACK threshold | packets per send | `SEND_ZC` | `sendmsg` | send_sys, `SEND_ZC` / `sendmsg` |
|---|---|---|---|---|
| 2 (default) | 2.1 | 3.70 Gbps | 3.78 Gbps | 0.74 / 0.69 s |
| 8 | 8.3 | 5.05 | 5.08 | 0.35 / 0.35 |
| 10 | 10.1 | 5.11 | 5.24 | 0.35 / 0.33 |
| 12 | 11.5 | 5.24 | 5.27 | 0.32 / 0.31 |
| 14 | 13.2 | 5.30 | 5.34 | 0.31 / 0.31 |
| 16 | 14.6 | 5.27 | 5.37 | 0.32 / 0.30 |

### How to read this

**`SEND_ZC` has a fixed cost per send and saves a copy per byte.** From 2 to 12 packets it
costs 130–150 ns more per send than `sendmsg`: the completion notification, the io_uring
submit and the slot bookkeeping. The copy it removes grows with the send, and with packets
freed at once the two meet at about 20 packets (27 kB). embr sends at most 16 packets
(21.6 kB), and 2 at the default ACK threshold, so on this device `SEND_ZC` costs more at
every size embr sends.

**When the device holds packets, the slot pool decides.** A slot stays busy until the
device frees its packets. With 100 µs of delay and 8 slots the sender can start one send
every 12.9 µs and sleeps in between (`waits` in the stats line counts these); 16 slots
halve the sleep. With 128 slots it does not wait, and `SEND_ZC` is level with `sendmsg`
at 8–10 packets and 6–19% cheaper at 12–16 (two runs; the table is the second).
`sendmsg`'s cost rises more under delay than `SEND_ZC`'s; the cause was not profiled.

**It moves little in a transfer.** At 8–16 packets per send a GiB is 57k–100k sends, so
150 ns per send either way is 0.009–0.015 s per GiB against about 0.8 s of sender CPU.
The transfers above show it: from threshold 8 up the two egress paths are within 2%.
GSO removed the per-send cost that the choice of egress acts on. Loopback copies every
`SEND_ZC`, so those transfers can show a tie at best.

**Limits.** One desktop, one kernel, a dummy device: no DMA mapping, no real
transmit-completion path, and a sender that never waits for an ACK. 100 µs is an assumed
delay, not a measured NIC. Which table a real NIC resembles is for the two-host run.

### Reproduce

```bash
bash bench/egress_cost.sh                                   # first table
DELAY=100us SEGS_LIST="2 8 10 12 14 16" REPS=3 bash bench/egress_cost.sh               # 8 slots
DELAY=100us SLOTS=128 SEGS_LIST="2 8 10 12 14 16" REPS=3 bash bench/egress_cost.sh     # 128 slots
for t in 2 8 10 12 14 16; do                                # transfers
    EMBR_QUIC_ACK_THRESH=$t ROWS="embrquicgso embrquicsmgso" CACHE_DISCIPLINE=keep bash bench/bench.sh loopback
done
```

The delay table was measured with `REPS=3` and the row list shown; its 16-slot column is
`SLOTS=16`. In a transfer the pool is set with `EMBR_QUIC_ZC_SLOTS` on the sender.

No root is needed: the script runs in a private user and network namespace. The delay rows
need `tc` and the `sch_netem` module (Fedora: `kernel-modules-extra`).

---

<a id="raw-data-archive"></a>
## §11 — Consolidated WAN source files

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

