# QUIC receive path: batched reads and UDP GRO — notes

Measured 2026-11-17/18 on the desktop with a patch in a scratch tree
(`2026-11-17-receive-path/harness/prototype-final.patch`). Raw output is in
`2026-11-17-receive-path/`.

## What the current tree does per received packet

One `select`, one `recvmsg`, one `ngtcp2_conn_read_pkt`, one pass through
`ngtcp2_conn_writev_stream`, and an ACK sent for every second packet. Receiver CPU per GiB on
loopback, by purpose (`receiver-profile.txt`, perf task-clock with kernel call chains):

| Purpose | Current | Patch |
|---|---|---|
| Total | 3.18 s | 1.21 s |
| Sending ACKs (syscall, UDP/IP output, loopback delivery) | 1.01 | 0.05 |
| `recvmsg` + `select` | 0.61 | 0.09 |
| embr loop, memcpy, clock | 0.34 | 0.10 |
| ngtcp2 | 0.19 | 0.07 |
| AEAD decrypt | 0.28 | 0.22 |
| SHA-256 of the file | 0.42 | 0.42 |
| File pages and file syscalls | 0.27 | 0.26 |

416k ACKs per GiB become 15k. The sender's GSO batches grow from 2.1 to 14.9 packets because
it sends what each ACK frees.

## The patch

1. `pump_once` reads first and calls `select` only when nothing was waiting.
2. `read_packets` reads until the socket is empty or 64 packets are in, feeds each to ngtcp2,
   and the ACKs are written once afterwards (RFC 9000 §13.2.2 allows processing several
   available packets before deciding to acknowledge).
3. `UDP_GRO` on the socket with a 64 KiB buffer: one read returns a run of same-size
   datagrams; `EMBR_QUIC_GRO=0` switches it off.
4. `run_handshake` and `recv_file` accept progress made in the same pump as the peer's
   close. Found by `Connect_AcceptsMatchingCertHash`, which failed without it.

## Loopback, 1 GiB, n = 5, median (`SEND_ZC` row / `sendmsg` row)

| Variant | Gbps | Whole-host CPU s | Packets per send |
|---|---|---|---|
| a. current tree | 3.69 / 3.73 | 5.39 / 5.81 | 2.1 |
| b. read before waiting, one packet per pump | 4.11 / 4.15 | 4.39 / 4.18 | 2.1 |
| c. reads batched up to 64, no GRO | 6.98 / 7.10 | 2.56 / 2.43 | 14.7 |
| d. GRO, one read per pump | 7.28 / 7.41 | 2.24 / 2.26 | 14.6 |
| e. GRO, batched up to 64 (the patch) | 8.03 / 8.03 | 2.08 / 2.08 | 14.9 |
| f. as e, cap 16 | 7.47 / 7.54 | 2.17 / 2.16 | 15.0 |
| g. as e, cap 256 | 8.18 / 8.26 | 1.80 / 1.84 | 15.0 |
| h. as e, `EMBR_QUIC_ACK_THRESH=10` | 8.03 / 8.10 | 1.96 / 1.94 | 15.1 |
| i. as e, sender without GSO | 5.05 / 5.30 | 3.43 / 3.47 | 1.0 |
| j. current tree again | 3.73 / 3.78 | 4.91 / 4.64 | 2.1 |
| k. embr TCP, for scale | 12.63 | 1.13 | — |

The ACK threshold switch no longer changes anything (h). With the patch the receiver uses
1.04 s of CPU in 1.07 s of wall time; SHA-256 is 0.42 s of it.

## veth, packets arriving one by one (`veth-*.txt`)

Sender A, router R, receiver B. A's device has no UDP segmentation offload, so every GSO
batch is cut into packets before it leaves; B's device runs NAPI and GRO. The sender uses
`sendmsg` (see the artifact below).

| Path | Current | Patch | Patch, GRO off |
|---|---|---|---|
| Unshaped, 1 GiB, n = 3, Gbps | 4.29–4.36 | 7.99–8.00 | 6.97–7.11 |
| 1 Gbit/s, 10 ms round trip, Mbit/s | 922–923 | 925 | 923–925 |
| 100 Mbit/s, 20 ms, 100-packet queue, 64 MB, n = 3, Mbit/s | 76–79 | 69–79 | 78–79 (cap 16) |

Device GRO gave the receiver 14.9 packets per read on the unshaped path.

Loss on both directions at a 20 ms round trip, seconds per transfer, median [min, max]:

| Loss | File | n | Current | Patch |
|---|---|---|---|---|
| 0.1% | 64 MB | 8 | 18.8 [14.5, 23.8] | 18.9 [16.5, 25.1] |
| 1% | 20 MB | 3 | 21.0 [20.7, 21.9] | 20.9 [20.5, 24.0] |
| 5% | 5 MB | 3 | 12.7 [11.6, 14.4] | 14.0 [13.0, 15.9] |
| 10% | 5 MB | 3 | 20.5 [20.2, 21.6] | 23.9 [23.6, 25.5] |

Throughput under loss is 2–30 Mbit/s in both: that is CUBIC without pacing, not the receive
path.

## Two things that looked like problems

**10% loss on loopback: 1.4 s becomes 7.9 s** (`loopback-loss10/`, 20 MB, 16 transfers).
The loss rule there sees a GSO batch as one packet, so it drops 16 packets at once, and with
a near-zero round trip every lost flight costs a 26 ms probe timeout. On veth with per-packet
loss and 20 ms the same loss rate costs 16%. Acknowledging the first K packets of each pump
early does not repair it and is worse elsewhere (`loopback-ack-early.txt`; 0.1% loss at
20 ms: 34.5 s against 18.9 s), so it is not in the patch.

**Device GRO does not merge packets from a `SEND_ZC` sender on veth**
(`veth-gro-by-sender-mode.txt`): 1 packet per read, against 14.8 from a `sendmsg` sender and
16 from a minimal UDP sender. io_uring marks its zero-copy packets "do not orphan", so veth
forwards them uncopied and GRO leaves them alone. A wire cannot carry that state, so a real
NIC should merge them; the two-host run confirms it through the `[quic-ingress]` line.

## Not measured

- A real NIC. On c5n.large the September receiver spent 5.74 s user and 3.47 s sys per GiB.
  The embr TCP receiver's 2.64 s user against nc's 0.27 s says about 2.4 s of that is
  SHA-256 on a CPU without SHA extensions, which this patch does not touch.
- Bursts on a shallow queue beyond the one 100-packet case above. embr has no pacer.
