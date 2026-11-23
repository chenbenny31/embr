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
