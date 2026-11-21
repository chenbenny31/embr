# Packets per send, slot pool and the locked-memory budget — notes

Measured 2026-11-18 on the desktop. Raw output and scripts are in
`2026-11-18-gso-cap-and-slots/`. The packet cap and read burst were set with two
experiment switches that are not in the tree (`harness/experiment-switches.patch`).

## The locked-memory budget

`RLIMIT_MEMLOCK` defaults to 8 MiB since Linux 5.16, and systemd sets the same. The desktop
has 8192 KiB soft and hard; September's AWS hosts logged `unlimited`. embr's `SEND_ZC` path
needs kernel 6.2, so a lower limit can only come from an admin or a container setting.

How the limit is counted (`memlock-accounting.txt`, a probe with liburing):

| Question | Result |
|---|---|
| How much can one process register with io_uring? | 8192 KiB; 8200 KiB is refused |
| Is it per process? | No, per user: with one process holding 5000 KiB, a second is refused 5000 KiB and granted 3000 KiB |
| Does `mlock()` by another program take from it? | No: with another process holding 6 MiB by `mlock()`, 8000 KiB still registers |
| Does `mlock()` in the same process take from it? | No: 6 MiB by `mlock()` and 6 MiB registered both succeed |
| Is `mlock()` itself per process? | Yes: two processes each lock 6 MiB; one process is refused 9000 KiB |
| Is the charge released at exit? | Not at once: a registration made straight after the holder exits is refused; 10 ms later it succeeds |

So only programs of the same user that register io_uring buffers compete, in practice other
embr processes. `memlock-budget-back-to-back.txt` is a first attempt that ran without pauses
and shows the last row's effect; do not read limits from it.

Slab per connection = slots × packets × 1350 bytes:

| Packets per send | Slots | Slab | Connections of one user in 8 MiB |
|---|---|---|---|
| 16 | 8 (current) | 169 KiB | 48 |
| 16 | 32 | 675 KiB | 12 |
| 32 | 8 | 338 KiB | 24 |
| 48 | 8 | 506 KiB | 16 |
| 48 | 32 | 2025 KiB | 4 |

## Packets per send, in transfers

Loopback, 1 GiB, n = 5, median. "Burst" is how many packets the receiver reads before it
acknowledges.

| Cap | Burst | Achieved | `SEND_ZC` Gbps | `sendmsg` Gbps | Whole-host CPU s |
|---|---|---|---|---|---|
| 16 | 64 (current) | 14.9 | 7.88 | 8.03 | 2.96 / 2.48 |
| 16 | 256 | 15.0 | 8.03 | 8.10 | 2.51 / 2.49 |
| 24 | 64 | 21.7 | 8.03 | 8.18 | 2.17 / 2.51 |
| 24 | 128 | 21.9 | 8.18 | 8.26 | 2.06 / 2.07 |
| 32 | 64 | 27.6 | 7.95 | 8.03 | 2.26 / 2.68 |
| 32 | 128 | 27.8 | 7.74 | 7.88 | 5.96 / 6.39 (something else was running) |
| 32 | 256 | 27.9 | 8.10 | 8.18 | 2.14 / 2.05 |
| 48 | 64 | 39.8 | 8.03 | 8.10 | 2.07 / 2.04 |
| 48 | 128 | 39.9 | 8.03 | 8.10 | 2.23 / 2.17 |
| 48 | 256 | 40.7 | 8.10 | 8.18 | 1.96 / 1.94 |

veth, packets arriving one by one, `sendmsg` sender, n = 3:

| Cap / burst | Unshaped, Gbps | 100 Mbit/s, 20 ms, 100-packet queue, Mbit/s | 1% loss, 20 ms, 20 MB, s |
|---|---|---|---|
| 16 / 64 | 7.80–7.88 | 73–78 | 22.0–23.6 |
| 32 / 128 | 7.85–7.98 | 69–78 | — |
| 48 / 256 | 7.96–8.04 | 69–73 | 21.7–24.3 |

## Slot pool, send-only, packets held 100 µs

`bench/egress_cost.sh` with `DELAY=100us`: the sender never waits for an ACK, so this is
the worst case for the pool. `SEND_ZC` minus `sendmsg`, CPU ns per send, and the wall time
of one `SEND_ZC` send:

| Slots | 16 packets | 24 | 32 | 47 | Wall per send |
|---|---|---|---|---|---|
| 4 | +253 | +139 | −72 | −356 | 25.8 µs |
| 8 | +166 | −90 | −226 | −465 | 12.9 µs |
| 16 | +47 | −82 | −229 | −465 | 6.5 µs |
| 32 | −138 | −378 | −566 | −854 | 3.2 µs |
| 64 | −180 | −358 | −499 | −839 | 1.6 µs |

With packets freed at once the two paths cross between 16 and 24 packets (+73, −69) and
`SEND_ZC` is 542 ns cheaper at 48.

## Reading

- **A larger cap buys almost nothing.** From 16 to 48 packets per send, throughput moves
  by 2–3% where the CPU is the limit, not at all at 1% loss, and slightly down on the
  shallow 100 Mbit/s queue. The receiver is the limit, and its cost is hashing and
  decryption.
- **`SEND_ZC` and `sendmsg` are level in transfers at every cap.** `sendmsg` is about 1%
  ahead on loopback, where every `SEND_ZC` is copied.
- **The pool matters only when sends outrun completions.** In transfers `waits` was 0
  with 8 slots. In the send-only blast, 32 slots or more make `SEND_ZC` 140–850 ns cheaper
  per send, which is 0.01–0.02 s per GiB at 20–55 thousand sends per GiB.
- **Defaults.** 16 packets and 8 slots (169 KiB, 48 connections per user) stay. Nothing
  here argues for a larger slab, and a larger one makes it more likely that a second
  connection of the same user falls back to `sendmsg`.

Not measured: a real NIC, where the hold time of a packet is whatever the device's
transmit-completion path gives. The two-host run reads it from `waits`.
