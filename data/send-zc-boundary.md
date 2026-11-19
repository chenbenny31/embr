# Where `SEND_ZC` pays against `sendmsg` — research notes

Working notes for a possible short paper, started 2026-11-17. The measurements are in the dated
directories beside this file and summarised in [bench.md §10](../bench/bench.md#egress-cost).

## Question

io_uring `SEND_ZC` removes a per-byte copy and adds a per-send cost (a second completion, the
io_uring submit, slot bookkeeping). At what send size does it start to pay against plain
`sendmsg`, and what moves that point?

## Published break-even sizes

| Source | Setting | Zero-copy starts to pay at | How it was checked |
|---|---|---|---|
| Jasny et al., "High-Performance DBMSs with io_uring" (PVLDB 19, arXiv 2512.04859), §4.6, Fig. 17 | TCP, one connection, cycles per byte, registered buffers | about 1 KiB | summaries only; the section itself was not read |
| Begunkov, io_uring zerocopy send patch series | UDP on a real NIC | between 1.5 and 4 kB (+0.4–4.5% at 600–1500 bytes, +22% at 4000) | search summary of the list thread |
| Kernel `Documentation/networking/msg_zerocopy.rst` | `MSG_ZEROCOPY`, TCP | about 10 kB | search result quoting the document |
| de Bruijn and Dumazet, LPC 2018 | `MSG_ZEROCOPY` on top of UDP GSO | 2–13% gain, large GSO sends only | slides |
| embr, [bench.md §10](../bench/bench.md#egress-cost) | UDP + GSO, 1350-byte packets, dummy device | about 27 kB (20 packets) | measured here |

Each source gives one number for one setup. No paper was found that measures `SEND_ZC` under
a QUIC stack (one vendor page makes claims without data). Not finding one is not proof.

## What embr measured

Desktop, kernel 7.3.3, Ryzen 9 9900X (L1d 48 KiB per core), one pinned core.

- **Fixed cost.** `SEND_ZC` costs 130–150 ns more per send than `sendmsg` from 2 to 12 packets
  (`2026-11-15-egress-cost/freed-at-once.txt`).
- **Where it goes** (`2026-11-15-egress-cost/old-harness/profile-by-category.txt`, 2 × 1200
  bytes): completion notification 69 ns; io_uring submit 69 ns against 50 ns of `sendmsg`
  entry; `ZcEgress` user code 48 ns against 9 ns; UDP/IP send path 235 ns against 202 ns.
  The copy it removes is 18 ns.
- **What the copy costs.** 110 ns for 16 × 1350 bytes, about 7 ns per packet. Past 24 packets
  it is about 10 ns per packet, and `sendmsg` also pays more for skb allocation (50 → 140 ns)
  and in the UDP/IP send path (212 → 414 ns) between 16 and 32 packets. That, not the copy
  alone, puts the crossing at about 20 packets.
- **Delayed completions.** With packets held 100 µs and 8 slots the sender waits 12.9 µs per
  send. With 128 slots it does not wait, and `SEND_ZC` is level at 8–10 packets and 6–19%
  cheaper at 12–16 (`held-100us-*slots.txt`).
- **Ring flags.** `SINGLE_ISSUER` and `DEFER_TASKRUN` change nothing when completions arrive
  inside the send call (`old-harness/pkt1200-ring-flags.tsv`).
- **In a transfer.** On loopback `SEND_ZC` is behind `sendmsg`: 3.69 against 3.73 Gbps at 2
  packets per send (`2026-11-17-default-settings/`), and 7.88 against 8.10 Gbps at about 15
  packets per send, the default since the receive path reads in batches
  (`2026-11-18-receive-path-applied/`, notes in `receive-path.md`).

## Why the published numbers may differ (hypotheses, not yet tested)

1. **Cache state of the payload.** A QUIC sender encrypts each packet just before sending it,
   so the kernel copies from memory already in L1. A database sending cold buffers pays far
   more per byte for the same copy. This would explain 1 KiB there and 27 kB here.
2. **A ceiling.** One UDP send carries at most about 64 kB, 47 packets of 1350 bytes, and
   ACK clocking keeps embr at 16 or fewer. The break-even is within a factor of 2.4 of the
   limit; the best case at the limit is 523 of 1562 ns per send. TCP has no such limit.
3. **Completion delay × slot pool.** A slot is busy until the device frees its packets, and
   the pool is bounded by the locked-memory limit (`ulimit -l`, shared by all of a user's
   processes). No published threshold accounts for this.

## Not yet explained

- `sendmsg` gets dearer under netem delay than `SEND_ZC` does (805 against 450 ns at 8
  packets). Not profiled.
- What sits in the extra 34 ns of UDP/IP send path that `SEND_ZC` pays at small sends.

## Experiments still missing, cheapest first

1. Hot against cold payload: a mode in `bench/egress_cost.cpp` that cycles through a slab
   larger than the cache. Tests hypothesis 1.
2. `MSG_ZEROCOPY` as a third arm, to tie the numbers to the 2018 result and the 10 kB figure.
3. A profile of `sendmsg` under delay.
4. A real NIC: the two-host AWS run, then a link fast enough that the sender is the limit.
5. Two or three kernels.

## Sources

- https://arxiv.org/pdf/2512.04859
- https://lkml.iu.edu/hypermail/linux/kernel/2502.2/05015.html
- https://lkml.iu.edu/hypermail/linux/kernel/2112.0/00493.html
- https://lwn.net/Articles/879724
- https://docs.kernel.org/networking/msg_zerocopy.html
- https://lpc.events/event/2/contributions/106/attachments/104/128/willemdebruijn-lpc2018-udpgso-presentation-20181113.pdf
- https://mailarchive.ietf.org/arch/msg/quic-issues/liSFWWmtDf0EcvhAMvrg9g7byAQ/
- https://quic.fastly.com/blog/measuring-quic-vs-tcp-computational-efficiency
- https://arxiv.org/html/2505.09222v1
