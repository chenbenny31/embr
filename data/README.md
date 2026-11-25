# data/ — raw measurements and research notes

Everything behind the `SEND_ZC` / `sendmsg` / GSO numbers lives here and is tracked by git.
[bench/bench.md](../bench/bench.md) holds the tables and how to read them; this directory
holds what they were computed from.

## Rules

- One directory per run, `YYYY-MM-DD-what/`, holding the output as the script wrote it.
- Text only: `.txt`, `.tsv`, `.log`. No binaries, no `perf.data`, no test files. Never
  `.out`: `.gitignore` drops it.
- Each results file starts with its date, kernel, commit and settings. Local paths are
  replaced by `$SCRATCH` (a build directory outside the repo) and `$REPO`.
- A run made on a commit marked `-dirty` names in the table below what was uncommitted.
- Notes that are not measurements go in a `.md` file at this level: `send-zc-boundary.md`, `receive-path.md`, `batch-size-and-slots.md`, `wan-2026-11-22.md`.

## Machine

The AWS hosts of `2026-11-22-wan/` are described in its `env.sender.txt` and `env.receiver.txt`.
Everything else: Fedora desktop, kernel 7.3.3-300.fc45, AMD Ryzen 9 9900X (12 cores, L1d 48 KiB and L2 1 MiB
per core, L3 64 MiB), `ulimit -l` 8192 KiB, `rmem_max`/`wmem_max` 4 MiB. Release build,
wolfSSL 5.9.2, ngtcp2 1.26.0.

## Index

| Directory | What | Backs | Command |
|---|---|---|---|
| `2026-11-24-file-io-and-connections/` | Whether the file end limits a QUIC transfer, asked before building an io_uring read-ahead / write-behind pipeline: source warm and cold, output on tmpfs and on an NVMe disk, 1 GiB (n = 5) and 16 GiB (n = 3) (`disk.txt`); and 1, 2 and 4 independent transfers at once as the most that more than one connection could give (`connections.txt`). Loopback, defaults | "Reading ahead and writing behind: the kernel already does both" below | `SCRATCH=<dir> bash harness/disk.sh 5 "label\|source\|output\|cold" ...`; `SCRATCH=<dir> FILE=<1 GiB file> bash harness/pairs.sh 5 1 2 4` |
| `2026-11-24-resume-check/` | How long a pull takes to find that the output is already complete, or to check a partial download before resuming it: one chunk at a time on one thread against sets of lanes on several threads (`check-times.txt`; 1 GiB, 1 GiB on two CPUs, 4 GiB; with and without SHA extensions). `638191f` against the tree with the change | section below | `harness/checkbench.sh` |
| `2026-11-23-sender-stall/` | What `wolfSSL_EVP_CipherInit` at 14% of the sender really is: the sender profiled with precise and with ordinary cycle sampling (`sender-profile.txt`), the next packet's source bytes prefetched and the file mapping populated (`prefetch.txt`), and the receiving thread's profile (`receiver-thread-profile.txt`). `638191f` plus `harness/experiment.patch` | section below | `harness/prof.sh`, `harness/prof-pull.sh`, `harness/capbench.sh` |
| `2026-11-23-defaults-only/` | Both ends with no `EMBR_*` variable set: `2c2b26d` against the tree where the connection chooses `SEND_ZC` or `sendmsg` itself and a send holds 48 packets. Delayed-path sweep (`delayed-path-first-version.txt`, `delayed-path-final.txt`), loopback (`loopback.txt`), and how long the probe may be: 16 sends against 3 (`probe-length.txt`) | section below | `harness/defsweep.sh`, `harness/vethdef.sh`, `harness/capbench.sh`, `harness/checks.sh` |
| `2026-11-23-short-sends-and-window/` | Short sends copied instead of queued for `SEND_ZC`, a 16 MiB flow-control window, the slot count following the batch size: the delayed-path sweep before and after (`rtt-sweep.txt`), loopback (`loopback.txt`), and the default socket-buffer ceiling measured again (`sockbuf-default-ceiling.txt`, which does not repeat the day before). `2c2b26d` against the tree with the change | section below | `harness/rttsweep.sh`, `harness/capbench.sh`, `harness/gate.sh` |
| `2026-11-23-packets-per-send/` | 16, 32 and 48 packets per send against the egress path, with the receiver that verifies on its own thread. Loopback transfers (1 GiB, n = 5, `loopback-cap.txt`), the veth 11 ms path and a shallow 100 Mbit/s queue (`veth-cap.txt`), and a profile of the sender that splits the send call into getting and zeroing pages, the copy and the rest (`sender-profile.txt`). `2c2b26d` plus the `EMBR_QUIC_GSO_SEGS` switch of `2026-11-18-gso-cap-and-slots/harness/experiment-switches.patch` | section below | `harness/capbench.sh`, `harness/sendprofile.sh` |
| `2026-11-22-delayed-path/` | The socket-buffer ceiling, and what else limits QUIC once a path has delay. Loopback and a veth A-R-B path with netem (rate 2.2 Gbit/s, 2 to 100 ms round trip), 1 GiB: embr's 4 MiB buffers against the kernel's default ceiling (`sockbuf-*.txt`), throughput against round-trip time (`rtt-scaling.txt`), the receiver's ACKs through `SEND_ZC` against `sendmsg` (`ack-egress.txt`, `receiver-ack-egress.txt`), flow-control windows of 4, 16 and 64 MiB (`fc-window*.txt`), 1 against 4 MiB chunks (`chunk-per-rtt.txt`). Files named `-zc-acks` were measured before the ACK stall was understood. The current tree plus `harness/experiment-switches.patch` (never applied) | table below | `SCRATCH=<dir> unshare --user --map-root-user --net --mount bash harness/vethsb.sh <rate> <delay each way> <limit> <file> <runs> <label>=<embr>[,ENV=VAL...] ...` |
| `2026-11-22-sha256-multi-buffer/` | SHA-256 of several chunks side by side with Intel's isa-l_crypto v2.26.1. `microbench.txt`: one thread, 1024 chunks of 1 MiB, OpenSSL per chunk against the library at 1 to 32 chunks per call and each of its CPU paths. `applied-loopback.txt`: 1 GiB transfers with the change applied, n = 5, the v0.9 behaviour, verifier threads chunk by chunk, and the default, QUIC and TCP, receiver pinned to one CPU, one core's two threads, 2 and 4 cores, with and without SHA extensions; `loopback.txt` the same from the prototype. `first-push-hash.txt`: the sender's first-push hash time. `checks.txt`. `harness/multi-buffer.patch` is the change as applied | table below | `g++ -O2 -std=c++20 harness/mbbench.cpp -I<prefix>/include <prefix>/lib/libisal_crypto.a -lcrypto`; `harness/abench2.sh` as in the row below with `EMBR_HASH_LANES=1` for chunk by chunk |
| `2026-11-22-verify-threads/` | More than one verifier thread, and two ideas that did not pay. `default-rule.txt`: threads = usable CPUs − 1, at most 4, against inline hashing, receiver pinned to one core's two threads and to 2, 3 and 4 cores, QUIC and TCP, with and without SHA extensions (loopback, 1 GiB, n = 5). `threads-and-pages.txt`: 1, 2 and 3 threads by switch, the receiving thread's own CPU, an output file whose pages already exist, and `MAP_POPULATE` on tmpfs and btrfs. `checks.txt`. The applied verifier thread plus `harness/verify-threads.patch` (the rule) or `harness/experiments.patch` (the switches, never applied) | table below | `SCRATCH=<dir> EMBR=<build>/embr bash harness/abench2.sh <quic\|tcp> <receiver cpu list> <mask sha 0\|1> 5 '<label>\|<ENV=value ...>\|<prefill 0\|1>' ...` |
| `2026-11-21-verify-thread/` | Chunk verification on a second thread (`ChunkVerifier` in `pull.cpp`, `EMBR_VERIFY=inline\|thread`): loopback, 1 GiB, n = 7, the same binary with the hash inline and on a thread, QUIC (`sendmsg` + GSO) and TCP, receiver pinned to two cores, to one core's two threads and to one CPU, with and without SHA extensions (`OPENSSL_ia32cap=:~0x20000000` hides them from the receiver's OpenSSL); `checks.txt` (tests, sanitizers, Helgrind); the patch as measured. `1bfdaf8` plus `harness/verify-thread.patch` | table below | `SCRATCH=<dir> bash harness/abench.sh <label> <quic\|tcp> <receiver cpu list> <mask sha 0\|1> 7` |
| `2026-11-22-wan/` | WAN run on AWS, us-east-1 → us-east-2, two c5n.large, kernel 6.18.62: four passes of `bench/wan.sh` (default with all seven rows, `gro-off`, `ack10`, `slots128`; 1 GiB, n = 10, both hosts' files, host records and NIC counters per pass), the smoke run, 16 single-row passes for the shaper counter, flat perf profiles of both hosts for six rows (`profiles/`), one and two plaintext streams (`path-ceiling-streams.txt`) and one and two embr transfers at once (`two-at-once.txt`). `1bfdaf8` plus the sender's wait for its ports in `bench/bench.sh`; network interface identifiers removed. `harness/`: the launch and teardown script, the follow-up passes, the two-at-once test and the table script | [`wan-2026-11-22.md`](wan-2026-11-22.md); bench.md §2 when it is updated | `SENDER_IP=<ip> SSH_KEY=<key> bash bench/wan.sh` on the receiver; `PASSES=custom CUSTOM='<row>\|\|' RUNS=3 WARMUP=1` for a single row |
| `2026-11-18-gso-cap-and-slots/` | Packets per send (cap 16, 24, 32, 48) × read burst × `SEND_ZC` slots: loopback transfers (1 GiB, n = 5), veth runs, send-only cost with packets held 100 µs at 4–64 slots, and a probe of how `RLIMIT_MEMLOCK` is counted for io_uring buffers. The tree with the receive-path change, plus two experiment switches (`harness/experiment-switches.patch`) | `batch-size-and-slots.md` | `harness/tune.sh`, `harness/run.sh` |
| `2026-11-18-receive-path-applied/` | The receive-path change as applied to the tree: every loopback row (1 GiB, n = 5), the two GSO rows with `EMBR_QUIC_GRO=0`, and 10% loss on loopback, 24 per mode. `bec6371` plus the wait counter and the receive-path change | a future bench.md section | `bash bench/bench.sh loopback`; `EMBR_QUIC_GRO=0 ROWS="embrquicgso embrquicsmgso" bash bench/bench.sh loopback` |
| `2026-11-17-receive-path/` | Receive-path prototype: batched reads and UDP GRO against the current tree. Loopback variants `a`–`k` (1 GiB, n = 5), receiver perf profile before and after, veth runs where packets arrive one by one (unshaped, rate-limited, loss at 20 ms round trip), 10% loss on loopback, the patch measured (`harness/prototype-final.patch`) and the scripts. `bec6371` plus the wait counter, with the patch applied in a scratch tree | `receive-path.md` | `harness/all.sh`, `harness/vethall.sh`, `harness/lossv.sh` |
| `2026-11-17-default-settings/` | `SEND_ZC` against `sendmsg` at embr's defaults: GSO on, ACK every 2 packets, 8 slots. `bec6371` plus the wait counter and `EMBR_QUIC_ZC_SLOTS` | table below | `ROWS="embrquicgso embrquicsmgso" RUNS=10 CACHE_DISCIPLINE=keep bash bench/bench.sh loopback`; `SEGS_LIST="1 2 4" bash bench/egress_cost.sh`; `DELAY=100us SEGS_LIST=2 bash bench/egress_cost.sh` |
| `2026-11-15-egress-cost/` | Cost of one send, 1 to 47 packets, freed at once and held 100 µs with 8, 16 and 128 slots | bench.md §10, first two tables | bench.md §10 "Reproduce" |
| `2026-11-15-egress-cost/old-harness/` | First harness (1200-byte packets, `sendmsg` / io_uring send / `SEND_ZC`, dummy and loopback), ring flags, 32 slots, and the perf profile split by what the code is for; sources included. Columns of the `.tsv` files: device, mode, ring flags, packet bytes, packets per flush, GSO, max segments, packets per send, wall ns per packet, sys, user, softirq, wall ns per send, copied %, errors | `send-zc-boundary.md` | `unshare --user --map-root-user --net python3 drive.py …`; superseded by `bench/egress_cost.sh` |
| `2026-11-15-ack-threshold-transfers/` | 1 GiB loopback transfers at ACK threshold 2, 8, 10, 12, 14, 16, both egress paths with GSO, n = 5. `bec6371` plus the wait counter | bench.md §10, third table | `EMBR_QUIC_ACK_THRESH=$t ROWS="embrquicgso embrquicsmgso" CACHE_DISCIPLINE=keep bash bench/bench.sh loopback` |
| `2026-11-15-gso-loopback/` | Egress × GSO, 1 GiB loopback, n = 10: `loopback_2x2` (1200-byte packets, threshold 2), `_ack10`, `_ack32`, `loopback_p1350` (1350-byte packets), `loopback_pkt_full0` (n = 5, 1350 bytes without the probe). `3783d38` plus the GSO work that became `bec6371` | bench.md §9 | `bash bench/bench.sh loopback` with `EMBR_QUIC_ACK_THRESH` as named |
| `2026-11-15-packet-loss/` | 20 MB transfers at 10% loss, 24 per mode, default pool and 2 slots; one line per transfer, `ok` and seconds. `bec6371` plus the wait counter | the check before the v0.8 tag | `loss4.sh` |

## Chunk verification on a second thread (2026-11-21)

Loopback, 1 GiB, n = 7, median Gbps, hash inline → hash on a second thread. Sender pinned to four
other CPUs. "No SHA ext" stands in for a CPU like the c5n's (SHA-256 at 980 MB/s here, 437 there).

| Receiver pinned to | SHA ext | QUIC | TCP |
|---|---|---|---|
| 2 cores | yes | 7.54 → 10.87 (+44%) | 12.27 → 17.53 (+43%) |
| 1 core, 2 threads | yes | 7.41 → 10.11 (+36%) | 12.27 → 17.53 (+43%) |
| 2 cores | no | 4.59 → 7.16 (+56%) | 6.09 → 7.28 (+20%) |
| 1 core, 2 threads | no | 4.59 → 6.01 (+31%) | 6.14 → 6.98 (+14%) |
| 1 CPU | no | 4.59 → 4.50 (−2%) | — |

One verifier thread caps throughput at the hash speed: 7.8 Gbps without SHA extensions here, which
is where the TCP rows stop. With one CPU the thread only adds switching, so embr verifies inline
there. The receiver's total CPU rises a little (1.10 → 1.14 s per GiB on two cores, 1.82 → 2.43 s
on one core's two threads). Not yet run on two hosts.

### More than one verifier thread (2026-11-22)

Same setup, n = 5, hash inline → verifier threads = usable CPUs − 1 (at most 4):

| Receiver pinned to | Threads | QUIC, no SHA ext | TCP, no SHA ext | QUIC, SHA ext | TCP, SHA ext |
|---|---|---|---|---|---|
| 1 core, 2 threads | 1 | 4.67 → 6.05 | 6.18 → 7.04 | 7.60 → 10.23 | 12.63 → 17.90 |
| 2 cores | 1 | 4.72 → 7.28 | 6.22 → 7.41 | 7.67 → 10.87 | 12.45 → 17.90 |
| 3 cores | 2 | 4.69 → 10.87 | 6.22 → 14.56 | — | — |
| 4 cores | 3 | 4.72 → 11.01 | 6.27 → 21.47 | 7.67 → 11.01 | 12.63 → 30.68 |

One thread hashes about 7.8 Gbps without SHA extensions and about 18 Gbps with them on this CPU;
the second and third lift the rows that were stopped there. QUIC stops near 11 Gbps whatever the
count: by then the receiving thread is busy 92% of the time (720 ms of CPU in 0.79 s).

Tried and dropped (`threads-and-pages.txt`): an output file whose pages already exist saves the
QUIC receiving thread 5% (721 → 684 ms per GiB) and nothing in throughput, so preparing pages on
another thread is not worth a thread; `MAP_POPULATE` on the chunk mapping changes nothing on tmpfs
(720 → 711 ms) and costs 12% on btrfs (664 → 744 ms).

### Hashing chunks side by side (2026-11-22)

One thread, 1 MiB chunks, MB/s (`microbench.txt`):

| | chunks per call | MB/s |
|---|---|---|
| OpenSSL, CPU without SHA extensions (stand-in) | 1 | 981 |
| OpenSSL, SHA extensions | 1 | 2,700 |
| isa-l_crypto, AVX2, 8 lanes | 8 or more | 2,400 |
| isa-l_crypto, AVX-512, 16 lanes | 16 or more | 7,850 |
| isa-l_crypto, AVX-512, 16 lanes | 8 | 4,020 |
| isa-l_crypto, AVX-512, 16 lanes | 1 | 605 |

Half-empty lanes are slow, so the verifier waits up to 200 ms for a full set and hashes fewer
than half a set one by one. Whole transfers with the change applied (`applied-loopback.txt`),
loopback, 1 GiB, n = 5, Gbps and the receiver's CPU seconds per GiB. "v0.9" hashes on the
receiving thread one chunk at a time, "threads" on verifier threads one chunk at a time, "default"
is what embr now does by itself:

| Receiver pinned to | | v0.9 | threads | default |
|---|---|---|---|---|
| 1 CPU, no SHA ext | QUIC | 4.62, 1.81 s | 4.59, 1.82 s | 9.76, 0.84 s |
| | TCP | 6.05, 1.37 s | 6.09, 1.38 s | 19.98, 0.41 s |
| 1 core, 2 threads, no SHA ext | QUIC | 4.54, 1.85 s | 5.97, 2.42 s | 10.35, 0.98 s |
| | TCP | 6.14, 1.37 s | 7.04, 1.53 s | 26.84, 0.46 s |
| 2 cores, no SHA ext | QUIC | 4.64, 1.81 s | 7.16, 1.89 s | 10.60, 0.89 s |
| | TCP | 6.14, 1.37 s | 7.34, 1.40 s | 31.81, 0.41 s |
| 4 cores, no SHA ext | QUIC | 4.59, 1.82 s | 10.60, 1.90 s | 10.48, 0.89 s |
| | TCP | 6.14, 1.37 s | 21.47, 1.44 s | 31.81, 0.41 s |
| 1 CPU, SHA ext | QUIC | 7.47, 1.11 s | 7.41, 1.11 s | 9.76, 0.84 s |
| | TCP | 12.63, 0.65 s | 12.63, 0.65 s | 20.45, 0.39 s |
| 1 core, 2 threads, SHA ext | QUIC | 7.60, 1.09 s | 10.23, 1.24 s | 10.87, 0.93 s |
| | TCP | 12.27, 0.66 s | 17.90, 0.79 s | 27.71, 0.45 s |
| 2 cores, SHA ext | QUIC | 7.60, 1.09 s | 10.74, 1.13 s | 10.87, 0.88 s |
| | TCP | 12.27, 0.66 s | 17.90, 0.72 s | 30.68, 0.41 s |
| 4 cores, SHA ext | QUIC | 7.60, 1.09 s | 10.60, 1.15 s | 10.60, 0.88 s |
| | TCP | 12.45, 0.66 s | 30.68, 0.68 s | 31.81, 0.41 s |

With one usable CPU there is no verifier thread; the chunks are then collected and hashed side by
side on the receiving thread. `loopback.txt` holds the same comparison from the prototype.

The sender hashes a file before its first push listens. Seconds until the port is bound for a
1 GiB file, chunk by chunk → side by side (`first-push-hash.txt`, n = 5):

| Sender pinned to | no SHA ext | SHA ext |
|---|---|---|
| 1 CPU | 1.26 → 0.26 | 0.54 → 0.23 |
| 1 core, 2 threads | 1.03 → 0.17 | 0.35 → 0.16 |
| 2 cores | 0.66 → 0.14 | 0.30 → 0.14 |
| 4 cores | 0.35 → 0.09 | 0.17 → 0.09 |

Side by side, one thread is enough everywhere and the hash stops being the receiver's largest
cost. These are AVX-512 numbers from a Ryzen 9 9900X; an AVX2-only CPU gets the 2,400 MB/s row,
and the c5n's Skylake has not been measured.

## Paths with delay: socket buffers, the receiver's ACKs, flow control (2026-11-22)

Emulated path: sender, router and receiver in three network namespaces over veth, netem on the
router with a 2.2 Gbit/s rate limit and the delay named; 1 GiB; AVX-512 desktop. It is a model,
not a WAN: packets arrive one at a time, so device GRO merges nothing.

**The receiver's ACKs must not go through `SEND_ZC`.** A zero-copy slot stays busy until the
kernel lets go of the packet, and netem holds a packet for the path delay. The pull has 8 slots,
so it could send 8 ACK packets per one-way delay and waited for a slot on 82% of its sends
(`receiver-ack-egress.txt`; every one of those small packets was copied anyway). Throughput was
1.09 MiB per round trip whatever the path could carry:

| Round trip | pull on `SEND_ZC` (default) | pull on `sendmsg` |
|---|---|---|
| 5.6 ms | 1,654 Mbit/s | 2,027 |
| 11.1 ms | 830 | 1,997 |
| 22.1 ms | 416 | 1,107 |
| 50.1 ms | 184 | 490 |

A real NIC frees a packet at transmit completion, so the AWS run (1.71 Gbit/s at 11 ms) was not
held this way; anything that queues packets on the receiver's way out (a shaping qdisc, a tunnel,
a container's veth) would be.

**Then the flow-control window.** With ACKs on `sendmsg`, embr's 4 MiB stream window allows about
3 MiB per round trip. Median of 2 runs, Mbit/s:

| Round trip | 4 MiB (embr's) | 16 MiB | 64 MiB |
|---|---|---|---|
| 22 ms | 1,106 | 1,786 | 1,780 |
| 50 ms | 489 | 522 | 572 |
| 100 ms | 245 | 263 | 263 |

At 50 ms and more the congestion window is the limit: slow start ends early (HyStart++) and
CUBIC grows slowly over a transfer this short.

**The socket-buffer ceiling.** embr asks for 4 MiB and gets 8,388,608 here; on a host with the
kernel's default `net.core.rmem_max`/`wmem_max` it gets 425,984. Asking for 212,992 gives exactly
that, so the default ceiling can be measured without root. Mbit/s, median of 5, and UDP
receive-buffer drops per transfer:

| | 4 MiB | default ceiling |
|---|---|---|
| Loopback, current receiver, 2 cores | 10,600, 0 | 10,110, 119 |
| Loopback, v0.9 receiver (hash inline), 2 cores | 7,410, 0 | 6,820, 164 |
| 11 ms path, current receiver, 2 cores | 2,000, 0 | 1,988, 300 (two runs of five at 1,217 and 1,624) |
| 11 ms path, current receiver, 1 core 2 threads | 1,998, 0 | 1,994, 20 (one run of five at 1,840) |
| 11 ms path, v0.9 receiver | 2,000, 0 | 321, 525 |

With the hash on the receiving thread the default ceiling overflowed on every chunk; with the
verifier thread it costs under 1% at the median and an occasional slower run. The 11 ms rows use
`sendmsg` for the pull's ACKs and no SHA extensions; `sockbuf-wanlike-zc-acks.txt` is the same
comparison on the stalled baseline (830 against 379 Mbit/s) and should not be quoted.

**Corrected on 2026-11-23: the 11 ms rows for the current receiver did not repeat.** The same
binary and script a few hours later dropped packets at the receive buffer in every run and gave
525-1,432 Mbit/s (`2026-11-23-short-sends-and-window/sockbuf-default-ceiling.txt`). Read those
two rows as "between 2,000 and a third of it", not as "under 1%".

## Reading ahead and writing behind: the kernel already does both (2026-11-24)

Question: would an io_uring pipeline (the sender reads batch n + 1 from the file while it sends batch n, the
receiver takes batch n + 1 off the socket while batch n is written) make a QUIC transfer faster? Measured
before building it, with the tree as it is (file mapped on both ends, one thread per connection).

| Loopback, defaults | Gbit/s |
|---|---|
| 1 GiB, source in the page cache, output on tmpfs | 12.45 |
| 1 GiB, source in the page cache, output on the NVMe disk (btrfs) | 12.10 |
| 1 GiB, source read from the disk while sending | 11.61 |
| 16 GiB, source read from the disk while sending, output on the disk | 12.56 [12.51, 12.60] |

The file end does not limit the transfer: the kernel's read-ahead stays in front of the sender as it walks
the mapping, and its writeback follows the receiver, with both ends on a disk and 16 GiB going through. What
the receiving thread spends on the file is the copy into the mapping (6%) and the page faults behind it
(about 8% in the profile of 2026-11-23; preparing the pages ahead recovered at most 5%, 2026-11-22). A disk
slower than the path would limit the transfer whichever way it is written. Not measured: such a disk, where
writing through a mapping makes the receiving thread wait inside a page fault instead of holding the sender
back through flow control.

What limits it is one thread per end doing the cipher, ngtcp2 and the UDP stack for every packet. Several
independent transfers at once, each on its own CPUs:

| At once | Total Gbit/s | Against one |
|---|---|---|
| 1 | 11.90 | |
| 2 | 22.44 | 1.89× |
| 4 | 38.59 | 3.24× |

That is the most that k connections for one transfer could reach on this machine. On the AWS path of
2026-11-22 the ceiling of 2.1–2.2 Gbit/s was per flow (two plaintext streams carried twice as much), so a
second connection is also the only way past it there; the two vCPUs of a c5n.large are one core's two
threads, which is why two embr transfers gained only 19% on that instance.

## The checks before a transfer hash side by side too (2026-11-24)

A pull that finds its output already there hashes every chunk to decide whether it is complete,
and a resumed pull hashes every chunk its bitmap records before trusting it. Both did that one
chunk at a time on one thread with OpenSSL, while received chunks have been hashed side by side
since `2c2b26d`. The change gives both the sender's parallel hash (`hash_chunks_parallel`, a
list of chunks instead of all of them), in rounds of one set of lanes per thread so that a
different file of the same size is given up after the first round.

Wall time of the whole pull, TCP on loopback, files in memory (`check-times.txt`; the resume case
has every chunk but the last recorded, and its times are the same within 0.02 s):

| Already complete | Before | After |
|---|---|---|
| 1 GiB | 0.47 s | 0.05 s |
| 1 GiB, no SHA extensions | 1.16 s | 0.05 s |
| 1 GiB, pull on one core's two threads | 0.47 s | 0.13 s |
| 1 GiB, the same, no SHA extensions | 1.16 s | 0.13 s |
| 4 GiB | 1.82 s | 0.17 s |
| 4 GiB, no SHA extensions | 4.55 s | 0.17 s |

CPU time does not fall on the 24-thread desktop (0.46 to 0.48 s for 1 GiB: 24 threads on 12 cores
share memory and AVX-512 units); on two threads it falls from 1.15 to 0.23 s.

Checks on the changed tree (`checks.txt`): 120 tests, also with `EMBR_HASH_LANES=1`; ASan and UBSan
clean in both; 12 rounds of 8 concurrent runs of the pull, hash and partial-file tests in both,
and of the QUIC suite, 96 of 96 each.

## The sender's 14% in `wolfSSL_EVP_CipherInit` is time spent waiting for the file (2026-11-23)

`perf record` picks `cycles:P` by default. On this CPU that event tags an op when it is
dispatched, so the time a stalled pipeline loses is charged to code issued after the stall. With
it the sender shows 14.5% of its cycles on four stores at the top of `wolfSSL_EVP_CipherInit`.
With ordinary sampling (`cpu/cycles/`, taken at retirement) that function is 1.5%, and the time
is on one instruction of `wc_AesGcmSetExtIV`, the first load of the packet's 12-byte nonce
(`sender-profile.txt`):

| Sender, ordinary sampling | Share of cycles |
|---|---|
| `wc_AesGcmSetExtIV`, one load | 10.5-13.1% |
| `memmove`, the payload from the file mapping into the packet | 9.0-10.0% |
| AES-GCM (encrypt, GHASH, init) | about 15% |
| Kernel zeroing pages for the send | 6.4-7.9% |
| Kernel mapping the file's pages (`next_uptodate_folio`, `zap_present_ptes`) | 5-6% |

ngtcp2 writes the nonce as a 12-byte copy and then an 8-byte store over its last 8 bytes; wolfSSL
reads it back as bytes 0-7 and 4-11. The first of those loads needs bytes from two stores, so
the CPU cannot hand it the value from the store buffer and it waits until every earlier store
has been written to the cache. The copy of the packet's payload is among those stores, and it
waits for the file's bytes to arrive from memory: the file is read 1,300 bytes at a time, and
the hardware prefetcher starts over at every 4 KiB page. So the load is where the wait for the
file shows, and neither library is slow in itself.

Prefetching the bytes of the next packet in `write_stream()` (`prefetch.txt`, n = 10):

| | Sender CPU, s per GiB | Gbit/s | Packets per send |
|---|---|---|---|
| As committed | 0.66 | 12.10 (11.61-12.45) | 39.5 |
| Prefetch 1,350-2,700 bytes ahead | 0.60 | 11.77 (11.45-11.93) | 36.0-36.2 |

The stall halves (10.5% of 3.72e9 cycles to 5.0% of 3.37e9) and the sender needs 9% less CPU,
but the transfer does not get faster: the receiving thread is the limit (about 99% of one CPU,
the sender 92% before and 82% after), and a sender that finishes its bursts sooner sends
smaller ones, which costs the receiver more reads. Prefetching 32 KiB ahead gains nothing, and
`MAP_POPULATE` on the mapping changes nothing (0.65 s, 12.10 Gbit/s). Not applied.

The receiving thread has no such single cost (`receiver-thread-profile.txt`): AES-GCM about 20%,
the kernel's copy out of the socket 6%, the copy into the output mapping 6%, spinlocks 7%, and
a long tail. Its output was on tmpfs here.

## Nothing set on either end: the connection chooses its egress (2026-11-23)

The version of the change that followed the section below, the same day. Three differences from it:
a send holds up to 48 packets (the most a UDP datagram carries) with no switch to raise it; a
connection leaves `SEND_ZC` for `sendmsg` when the kernel reports that it copied each of the
first sends (`QUIC_ZC_PROBE_SENDS`); and `EMBR_QUIC_ZC_MIN` is gone, `EMBR_QUIC_EGRESS=zc` forcing
every send through `SEND_ZC` instead. Short sends still go by `sendmsg`, the window is 16 MiB.

Both ends started with no `EMBR_*` variable but `EMBR_QUIC_STATS`. Same emulated path (veth
A-R-B, netem, 2,200 Mbit/s, queue of 3,000 packets), 1 GiB; loopback n = 5, median:

| Path | `2c2b26d` | Changed tree |
|---|---|---|
| Loopback | 10.23 Gbit/s | 12.45 Gbit/s |
| 5 ms round trip | 461 Mbit/s | 2,031 |
| 10 ms | 236 | 2,006 |
| 20 ms | 119 | 1,908 (1,755-1,934) |
| 50 ms | 48 | 524 |

The left column is far below the "before" of the section below because there the push was put
on `sendmsg` by hand. Left alone, the push at `2c2b26d` is held by its own slots on this path:
netem keeps each packet, and with it the slot, for the one-way delay, so 8 slots of 16 packets
leave per delay (`waits` on 54,000 of 55,000 sends). A real NIC releases a slot when the packet
is transmitted; on AWS (2026-11-22) 3% of GSO sends waited and nothing was copied. The gain on
this table belongs to paths where the kernel copies or holds packets: loopback, container
networks, shaping qdiscs.

**How long the probe may be** (`probe-length.txt`). With a probe of 16 sends and 48 packets per
send the changed tree was 5% slower on the 20 ms path than with 16 packets per send (median
1,831 against 1,919 Mbit/s, 2,500 more packets retransmitted). The batch size was not the
cause: with `SEND_ZC` off at both ends the two were equal (1,921 and 1,923). At 48 packets a
connection has 3 slots, and on a path that holds packets the 16 answers come back three per
one-way delay, the sender waiting for a slot 4 to 7 times while it should be reading ACKs. With
a probe of 3 sends (no more than the slots) there are no waits and the two are equal again
(1,933 and 1,919). On loopback the probe length does not show (12.45 Gbit/s either way).

Checks on the final tree (`checks.txt`): 115 tests in the default mode and with
`EMBR_QUIC_EGRESS=zc`, `EMBR_QUIC_GSO_SEGS=16` (111 to 114 where tests skip themselves:
`EMBR_QUIC_EGRESS=sendmsg`, `EMBR_QUIC_GSO=0`, `EMBR_QUIC_GRO=0`); ASan and UBSan clean in three
modes; the load gate 96 of 96 in four modes. The branch that leaves `SEND_ZC` is covered by a
unit test on loopback; a path that holds packets is covered by the harness here, not by a test.

## Short sends copied, a 16 MiB window: before and after (2026-11-23)

Superseded the same day by the version in the section above; the numbers here are for this
version. The change: a send below 1,200 bytes goes out with `sendmsg` even when `SEND_ZC` is on (so the
pull's ACKs take no slot), the stream window is 16 MiB (connection 32 MiB), the packets per send
are a switch (`EMBR_QUIC_GSO_SEGS`, default 16), and the slot count follows the batch size so
the registered slab stays near 169 KiB. Same emulated path as the section above, push on
`sendmsg` (netem would hold a `SEND_ZC` sender's slots as it held the receiver's), pull at its
defaults. Mbit/s, three runs each, all within 1% of each other (`rtt-sweep.txt`):

| Round trip | Before | After |
|---|---|---|
| 5.6 ms | 1,657 | 2,029 |
| 11.1 ms | 828 | 2,002 |
| 22.3 ms | 416 | 1,819 |
| 50.2 ms | 182 | 518 |

The receiver's egress line went from `sends=14916 waits=12201` to `sends=377968 waits=0
small=377965`. With the window no longer the limit the sender fills the router's 3,000-packet
queue and loses 0.4-0.8% of its packets there, as a loss-based sender does; before, flow control
kept it under the path's rate and nothing was lost.

Loopback, 1 GiB, n = 5, median Gbit/s (`loopback.txt`): unchanged at the defaults, 10.23 before
and after with `SEND_ZC`, 10.74 before and after with `sendmsg`. At 48 packets per send: 11.30
(`SEND_ZC`, 3 slots) and 12.45 (`sendmsg`).

**The default socket-buffer ceiling, again** (`sockbuf-default-ceiling.txt`, 11 ms path, 10 runs
each over two sets, Mbit/s, median and range):

| | Median | Range |
|---|---|---|
| Before, 4 MiB window | 978 | 645-1,432 |
| Before, 16 MiB window | 794 | 658-1,303 |
| After | 767 | 525-929 |

Every run lost 200-940 packets at the receive buffer. The buffer the kernel grants (425,984
bytes of accounted memory) holds about a millisecond of a 2 Gbit/s stream, so any pause of the
receiving thread overflows it, and each overflow is a separate loss event for the sender. With
embr's own buffers the same receiver is at 56% of one CPU on this path and drops nothing. Why
the day before was mostly free of drops is not known; full swap was ruled out (set 2). The
larger window is not what hurts (the two "before" rows overlap), and this path is harder on the
receiver than a NIC is: every packet is its own read.

Checks on the changed tree: 114 tests in the default mode and with `EMBR_QUIC_EGRESS=sendmsg`,
`EMBR_QUIC_GSO=0`, `EMBR_QUIC_GRO=0`, `EMBR_QUIC_ZC_MIN=0`, `EMBR_QUIC_GSO_SEGS=48`; ASan and
UBSan clean; the load gate (12 rounds of 8 suites at once) in four modes, results in
`checks.txt`. Before the slot count followed the batch size, 2 of 96 gate runs at 48 packets
per send could not register their slab (506 KiB each, against the 8 MiB a user may lock) and
fell back to `sendmsg`.

## Packets per send, again, and what the copy costs the sender (2026-11-23)

On 2026-11-18 a larger send bought 2-3%, because the receiver was the limit
([`batch-size-and-slots.md`](batch-size-and-slots.md)). Since the receiver verifies on its own
thread the sender is the limit on loopback, and the same change is worth more. Loopback, 1 GiB,
n = 5, median, Gbit/s (`2026-11-23-packets-per-send/loopback-cap.txt`):

| Packets per send (cap, achieved) | `sendmsg` | `SEND_ZC` (copied: loopback) |
|---|---|---|
| 16, 14.9 (current) | 10.74 | 10.23 |
| 32, 27.7 | 12.10 | 11.01 |
| 48, 40 | 12.45 | 11.45 |
| 48, 40, receiver reads 256 before it acknowledges | 12.63 | 11.61 |

Part of the gain is loopback's: a send arrives as one read, so the receiver's reads shrink with
the sender's. On the veth paths nothing moves (`veth-cap.txt`): 1,995-2,004 Mbit/s at every
size on the 11 ms / 2.2 Gbit/s path, 74-78 Mbit/s on a 100 Mbit/s, 20 ms, 100-packet queue,
with the same number of retransmitted packets.

**What a copying send costs.** Profile of the push process during a loopback transfer
(`sender-profile.txt`), percent of its samples:

| | `sendmsg`, 16 | `sendmsg`, 48 |
|---|---|---|
| The send call, whole | 26.4 | 19.7 |
| Getting pages for the payload and zeroing them (`sk_page_frag_refill`) | 9.0 | 10.3 |
| Copying the payload into them (`ip_generic_getfrag`) | 3.9 | 3.1 |
| The rest of the send, loopback's receive side included | 13.4 | 6.3 |
| wolfSSL's cipher setup per packet (`wolfSSL_EVP_CipherInit`) | 16.0 | 16.0 |

This kernel zeroes every page it allocates (`CONFIG_INIT_ON_ALLOC_DEFAULT_ON=y`), and that
zeroing is twice the copy. A send that references the caller's pages skips both, so 13% of the
sender's cycles is the most `SEND_ZC` could save here, on a device that does not copy. The
send-only numbers in bench.md §10 put it at 1-2% at 47 packets per send and below zero at 16:
there the device frees each page at once and the next send gets it back warm. It is also the likely cause of the line in §10 that
`sendmsg` gets dearer when packets are held. A whole transfer without the copy cannot be run
on one machine: loopback and veth hand the packet to a local socket and the kernel copies it
first (`skb_copy_ubufs`, 17% of the sender in the `zc16` profile).

Looked into the same day: `wolfSSL_EVP_CipherInit` as the largest single symbol of the sender (14% self) is an effect
of the sampling event, see the section on it above. The page-zeroing and copy rows of this profile were taken
with the same event and may be shifted too; with ordinary sampling the zeroing is 6.4-7.9%.

## `SEND_ZC` against `sendmsg` at the default settings (2026-11-17)

Measured before the receive path read in batches. Since 2026-11-18 the defaults give about 15
packets per send, not 2.1, and the same two rows read 7.88 Gbps (`SEND_ZC`) against 8.10
(`sendmsg`): see `2026-11-18-receive-path-applied/`.

Whole transfers, loopback, 1 GiB, n = 10, median [min, max]:

| | `SEND_ZC` | `sendmsg` |
|---|---|---|
| Throughput, Gbps | 3.69 [3.62, 3.73] | 3.73 [3.72, 3.80] |
| Sender kernel time, s | 0.725 [0.69, 0.78] | 0.675 [0.64, 0.70] |
| Whole-host CPU, s | 5.17 [4.82, 5.83] | 4.93 [4.66, 5.72] |
| Packets per send | 2.1 | 2.1 |

Loopback copies every `SEND_ZC`, so it can only show the overhead. `waits` was 0.

One send of 2 packets, dummy device (nothing copied), nanoseconds of CPU, median of 5:

| | `SEND_ZC` | `sendmsg` |
|---|---|---|
| Packets freed at once | 522 | 372 |
| Packets held 100 µs, 8 slots | 986, and 12.9 µs of wall time per send | 555, wall 0.6 µs |

On a real NIC with GSO on (2026-11-22, [`wan-2026-11-22.md`](wan-2026-11-22.md)): 0 datagrams copied, and the two
break even at 15 packets per send, 2.27 s (`SEND_ZC`) against 2.33 s (`sendmsg`) of sender CPU per GiB.
September's WAN run (no GSO, 1200-byte packets) showed 0 copied and no sender CPU saving.
