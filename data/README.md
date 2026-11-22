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
