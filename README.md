# embr
A zero-copy large file transfer engine built with C++20: `sendfile` + `splice` on TCP, a QUIC transport (ngtcp2 + wolfSSL, TLS 1.3) with an io_uring `SEND_ZC` egress, and a pluggable transport layer that the protocol code never sees through.

*From Old English ǣmyrġe, "smoldering ash." A shared file is like an ember: still glowing, passed from hand to hand, never fully extinguished.*

## Why

Existing tools arrange disk I/O and network I/O sequentially — the disk waits for the network, the network waits for the disk. Add redundant memory copies between kernel and userspace, and even 10G links stay half-idle.

embr pipelines disk and network operations in parallel and eliminates intermediate copies, targeting near-line-rate throughput at minimal CPU overhead. The same protocol code drives every transport; what changes underneath is who owns a buffer and when it may be released.

## Two Transfer Modes

**Trusted network (LAN / datacenter)** — TCP with zero-copy I/O:
- `sendfile()` on push (0 copies), `splice()` on pull (0 copies)
- `TCP_NODELAY` on control messages, `SO_SNDBUF`/`SO_RCVBUF` OS-autotuned

**Public network (encrypted)** — QUIC over ngtcp2 + wolfSSL:
- TLS 1.3 handshake, 30 s idle timeout, ICMP soft errors tolerated
- Chunks are `mmap`'d and retained until the peer acknowledges them; ngtcp2 re-sends lost data from the retained mapping, no copy
- Datagrams leave through an io_uring `SEND_ZC` egress: a registered per-connection slab, each packet slot retained until the NIC's completion notification
- Falls back to `sendmsg` when the slab cannot be registered or the kernel lacks `SEND_ZC` (`EMBR_QUIC_EGRESS=sendmsg` forces it)
- Not yet: server-certificate verification (client accepts any cert), NAT traversal, tracker over QUIC

## Usage

```bash
# Direct mode — push listens, pull connects by IP
embr push <file> [--port PORT]
embr pull <ip>   [--port PORT] [--out PATH]

# QUIC — same commands, one flag; push needs a cert/key pair (self-signed is fine)
embr push <file> --transport quic --cert cert.pem --key key.pem
embr pull <ip>   --transport quic --out PATH

# Tracker mode — push registers a token, pull resolves it
embr push <file> --tracker http://<tracker-host>:10009
embr pull <token> --tracker http://<tracker-host>:10009

# Trust a tracker — persists URL, no --tracker flag needed after
embr trust http://<tracker-host>:10009
embr push <file>     # auto-registers token
embr pull <token>    # auto-resolves via trusted tracker

# Run your own tracker
embr tracker [--bind ADDR] [--port PORT] [--ttl MINUTES]

# Manage trusted tracker
embr trust --show    # print current trusted tracker
embr trust --clear   # remove saved tracker

# Diagnostics
EMBR_QUIC_STATS=1 embr push ...   # one [quic-egress] line per connection: sends, notifs, copied, fallback, errors
EMBR_QUIC_LOG=1   embr pull ...   # ngtcp2 packet log
```

## How It Works

**v0.8 — one buffer type, two release events**
```
QUIC send path                                   who releases, and when
  chunk = mmap(file range)                        Buffer{ptr, len, [base, map_len]{ munmap }}
  unacked_.push_back(move(chunk))                  retained until ngtcp2 reports the peer's ACK
  ngtcp2 assembles packets in place → slot         Buffer{slot, n, [this, i]{ release(i) }}
  inflight_[i] = move(packet); SEND_ZC submit      retained until the kernel's NOTIF completion
  ACK callback:  unacked_.pop_front()  → munmap
  NOTIF CQE:     inflight_[i] = Buffer{} → slot back to the free list
```
`push.cpp` / `pull.cpp` call `send_file` / `recv_file` and never see a `Buffer`. The same value type carries both owners; the retention structure differs, the release contract does not.

**v0.6 — token + tracker**
```
Sender                        Tracker (<tracker-host>:10009)     Receiver
  │  embr push file.bin                                         │
  │  → precompute SHA256 (cached in .embr.hash)                 │
  │  → tcp_listen(:10007)                                       │
  │──── POST /register ──────>│                                 │
  │<─── 200 {sender_ip_recorded} ───────────────────────────────│
  │  [push] token: 67c49d1e7b5c9a6a                             │
  │  → waiting for connection                                   │
  │                            │<──── GET /resolve/:token ───── │
  │                            │───── {sender_ip, port} ──────> │
  │<══════ TCP transfer (request-driven, resume-capable) ══════>│
  │──── POST /unregister ─────>│                                │
```

**v0.5 — request-driven transfer with resume**
```
Sender                                                        Receiver
  │  embr push file.tar.gz                                        │
  │  → precompute SHA256 for all chunks                           │
  │  → listening on :10007                                        │
  │  ←──────────── HANDSHAKE ──────────────────── [TCP] ────────  │
  │  ──── FILE_META {filename, size, chunk_hashes[0..N]} ───────→ │
  │                                      check .embr.partial      │
  │  ←──────────── CHUNK_REQ{i0} ─────────────── [TCP] ────────   │
  │  ←──────────── CHUNK_REQ{i1} ─────────────── [TCP] ────────   │
  │  ←──────────── COMPLETE ───────────────────── [TCP] ────────  │
  │  ──── CHUNK_HDR{i0} + sendfile(chunk i0) ──────────────────→  │
  │                                      verify SHA256 → save     │
  │  ──── CHUNK_HDR{i1} + sendfile(chunk i1) ──────────────────→  │
  │                                      verify SHA256 → save     │
  │  ...                                                          │
```

Push answers requests and holds no session state — stateless across connections. Resume state lives entirely on the pull side in `.{filename}.embr.partial`. Token is derived from file content (`SHA256(concat(chunk_hashes))`), first 8 bytes hex-encoded — same file always produces same token.

## Benchmark

See [bench/bench.md](bench/bench.md) for methodology, raw per-run data and reproduction commands. Every number below is from one campaign on 2026-09-06: two `c5n.large` instances, Amazon Linux 2023 kernel 6.18, us-east-1 → us-east-2, 1 GiB random file, page cache dropped on both ends before each transfer, 2 warmups then 10 timed rounds, every transfer SHA-256 verified. Release build.

### Zero-copy mechanism (strace syscall count, 1 GB)

| process | dominant syscalls | total syscalls |
|---------|-------------------|----------------|
| embr push | `sendfile` ×1024 | ~7,408 |
| embr pull | `splice` ×4096 + `mmap` ×1024 (SHA-256 verify) | ~15,532 |
| ncat sender | `read` ×131,072 + `sendto` ×131,072 + `fcntl` ×524,288* | ~917,656 |
| ncat receiver | `recvfrom` ×131,072 + `write` ×131,072 | ~393,430 |

\* ncat artifact, not protocol work.

**Bytes through userspace on the TCP transfer path: embr = 0. ncat = 1 GB.** sendfile/splice move pages entirely in-kernel; ncat copies every byte through an 8 KB userspace buffer. Integrity hashing reads the mapped data separately on both ends. The trace confirms the path; timings come from untraced runs.

### WAN cross-region (1 GiB, AWS c5n.large us-east-1 → us-east-2, n=10, medians)

| tool | throughput | wall | receiver user / sys | sender user / sys |
|------|-----------|------|---------------------|-------------------|
| embr TCP (sendfile / splice) | 2.00 Gbps | 4.29 s | 2.64 s / 0.72 s | 0.00 s / 0.32 s |
| nc | 2.22 Gbps | 3.87 s | 0.27 s / 1.06 s | — |
| scp† | 1.12 Gbps | 7.73 s | 0.96 s / 1.94 s | — |
| embr QUIC, `SEND_ZC` egress | 0.91 Gbps | 9.41 s | 5.74 s / 3.47 s | 2.77 s / 2.97 s |
| embr QUIC, `sendmsg` egress | 0.905 Gbps | 9.51 s | 5.81 s / 3.59 s | 2.94 s / 2.56 s |

†scp is SSH-encrypted — the reference for the QUIC rows, not an I/O comparison for the TCP row.

**TCP path.** embr TCP and nc overlap within run-to-run spread (embr 1.81–2.19 Gbps, nc 2.00–2.74); nc's median is 10% higher, embr verifies every chunk. Receiver kernel time is 32% lower for embr (0.72 s vs 1.06 s), and the sender's kernel time per GiB is 0.32 s — `sendfile` hands file-cache pages to the socket and the sender process barely enters the kernel. Receiver user time (2.64 s) is SHA-256 verification, work nc does not do.

**QUIC path.** 0.91 Gbps on a path that carries 2 Gbps: the receiver is CPU-bound on one core, 9.2 s of CPU per GiB — 2.6 s SHA-256 (the TCP row pays the same), about 3 s decryption plus per-packet QUIC processing, and 3.5 s of kernel time from roughly two system calls per 1350-byte datagram. Per-packet cost, not copying, is the bottleneck. The v0.9 receiver work (batched `recvmmsg`, hashing off the connection thread) brings a loopback pull from 2.70 s to 1.95 s on a scratch build and is not yet in the tree.

**`SEND_ZC` result.** The egress counters report **0 of 11,243,123 datagrams copied by the kernel** across the campaign — real zero-copy on the ENA NIC. It did not reduce sender CPU: kernel time rose 0.4 s per GiB and user time fell 0.2 s. At 1350 bytes per datagram the completion bookkeeping costs more than the copy it removes; the copy was about 1% of per-packet work. Stated as measured. The lever that makes zero-copy pay at this granularity is segmentation offload (one submit per 64 KB), planned for v0.9.

**Cost of the ownership abstraction.** `bench/erasure_cost.cpp`: a `Buffer` lifetime with its type-erased release costs 11.5 ns on a laptop and 19.5 ns on the c5n sender, versus 0.7 / 1.2 ns for a function pointer with a context, with zero heap allocations. On the per-datagram path that is 10–17 ms per GiB, under 0.4% of any transfer above.

## Build
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## Install Dependencies

**Fedora / RHEL / Rocky:**
```bash
sudo dnf install cmake ninja-build gcc-c++ openssl-devel liburing-devel
```

**Ubuntu / Debian:**
```bash
sudo apt install cmake ninja-build g++ libssl-dev liburing-dev
```

**QUIC (wolfSSL + ngtcp2):** built from source into `~/.local/{wolfssl,ngtcp2}` by `bench/setup_quic_deps.sh`, which also raises `net.core.{r,w}mem_max`. wolfSSL must be configured with `--enable-aesgcm-stream`; without it every packet takes a hidden heap copy inside the EVP layer. The script enforces this.

**Kernel:** Linux 6.2+ for the io_uring `SEND_ZC` egress (`IORING_SEND_ZC_REPORT_USAGE`); older kernels fall back to `sendmsg` automatically. 6.0+ for the io_uring UDP path's `RECV`/`WRITE_FIXED`.

**memlock:** the QUIC egress registers about 105 KiB per connection (64 packet slots plus the ring); the io_uring UDP path registers its chunk pool. The default 8 MiB `ulimit -l` is sufficient; if registration fails, embr falls back rather than pinning per datagram.

## Architecture
```
embr/
├── src/
│   ├── main.cpp                        # verb dispatch (~20 lines)
│   ├── cli/
│   │   ├── push_cli.hpp/.cpp           # push argparse, token derivation, tracker register, --transport
│   │   ├── pull_cli.hpp/.cpp           # pull argparse, token detection, tracker resolve, --transport
│   │   ├── tracker_cli.hpp/.cpp        # tracker argparse, run_tracker_server
│   │   ├── trust_cli.hpp/.cpp          # trust argparse, ~/.config/embr/tracker
│   │   └── bench.hpp/.cpp              # bench argparse, --role sender/receiver
│   ├── core/
│   │   ├── protocol.hpp/.cpp           # send_msg/recv_msg, wire format, Buffer
│   │   ├── chunk_manager.hpp/.cpp      # runtime bitmap of completed chunks
│   │   ├── partial_file.hpp/.cpp       # .embr.partial serialize/deserialize
│   │   ├── hash.hpp/.cpp               # SHA256 + parallel pre-hash + .embr.hash cache
│   │   ├── push.hpp/.cpp               # sender logic       — unchanged since v0.5
│   │   └── pull.hpp/.cpp               # receiver logic, resume — unchanged since v0.5
│   ├── tracker/
│   │   ├── token_store.hpp/.cpp        # in-memory token→(ip,port) map + TTL
│   │   ├── tracker_handlers.hpp/.cpp   # HTTP handler logic
│   │   ├── tracker_server.hpp/.cpp     # cpp-httplib wiring, SIGINT shutdown
│   │   └── tracker_client.hpp/.cpp     # register/resolve/unregister HTTP client
│   ├── transport/
│   │   ├── transport.hpp               # abstract interface (frozen)
│   │   ├── tcp_transport.hpp/.cpp      # sendfile() push, splice() pull
│   │   ├── tcp_client.hpp/.cpp         # tcp_connect() factory
│   │   ├── tcp_server.hpp/.cpp         # tcp_listen() / tcp_accept() / tcp_from_fd()
│   │   ├── quic_transport.hpp/.cpp     # ngtcp2 conn pump, mmap retention until ACK, packet seam
│   │   ├── quic_egress.hpp/.cpp        # io_uring SEND_ZC slab: slot owner until NOTIF, fallbacks
│   │   ├── quic_client.hpp/.cpp        # quic_connect() factory, wolfSSL client ctx
│   │   ├── quic_server.hpp/.cpp        # quic_listen() / quic_accept(), wolfSSL server ctx
│   │   └── udp_transport.hpp/.cpp      # io_uring UDP data plane (experimental, not wired to the CLI)
│   └── util/
│       ├── socket_fd.hpp               # RAII fd wrapper
│       ├── exact_io.hpp                # send_exact/recv_exact
│       ├── json_parser.hpp             # hand-rolled flat JSON helpers
│       ├── config_tracker.hpp          # resolve_tracker_url, read/write config
│       ├── io_uring_ctx.hpp/.cpp       # io_uring ring + registered buffer pool (UDP path)
│       └── constants.hpp               # CHUNK_SIZE, HASH_SIZE, EMBR_PORT, TRACKER_PORT
├── bench/
│   ├── bench.md                        # methodology, raw per-run data
│   ├── setup_quic_deps.sh              # wolfSSL + ngtcp2 with the required flags, sysctl, SEND_ZC preflight
│   ├── tcp_loopback.sh / tcp_wan.sh    # TCP path benchmarks
│   ├── quic_loopback.sh / quic_wan.sh  # QUIC path benchmarks, SEND_ZC vs sendmsg rows, egress counters
│   └── erasure_cost.cpp                # microbench: cost of the type-erased Buffer release
├── tests/
│   ├── test_protocol.cpp, test_tcp.cpp, test_udp.cpp, test_resume.cpp, test_tracker.cpp
│   ├── test_hash_parallel.cpp          # parallel vs serial SHA-256 correctness
│   ├── test_quic.cpp                   # handshake, echo, send_file/recv_file, ACK retention, egress path
│   └── test_egress.cpp                 # ZcEgress slot lifecycle, drain, destructor
└── CMakeLists.txt
```

Business logic (`push`, `pull`) talks only to `Transport&` — never to raw sockets. Transport lifecycle is owned by the CLI layer. `run_push` / `run_pull` are byte-identical from v0.5 through v0.8: the splice receiver (v0.6), the QUIC transport (v0.8) and the `SEND_ZC` egress (v0.8) landed without touching `push.cpp` or `pull.cpp`. One design that did not fit — an io_uring UDP path that needed a second `Transport&` for its data plane — was withdrawn rather than accommodated.

`Buffer` (in `protocol.hpp`) is the other boundary: a move-only value with a pointer, a size, and a type-erased release. Its definition is unchanged since v0.3 while three owners were added underneath it — heap control messages via `unique_ptr`, mmap chunks released on the peer's acknowledgement, and `SEND_ZC` packet slots released on the kernel's notification.

## Wire Protocol
```
Header (6 bytes, always): [version:u8][type:u8][payload_len:u32 BE]

Message types:
  HANDSHAKE  (0x01)  [token_len:u32 BE][token:utf8]
  FILE_META  (0x02)  [file_size:u64 BE][filename_len:u32 BE][filename:utf8]
                     [chunk_size:u32 BE][chunk_count:u32 BE]
                     [chunk_hash[0]:32B]...[chunk_hash[N-1]:32B]
  CHUNK_REQ  (0x03)  [chunk_index:u32 BE]  — pull → push, one per needed chunk
  CHUNK_HDR  (0x04)  [chunk_index:u32 BE]  — push → pull, precedes raw chunk data
  COMPLETE   (0x06)  (no payload)          — pull → push, end-of-requests terminator
  ERROR      (0x07)  [reason:utf8]
  CANCEL     (0x08)  (no payload)
```

The same messages run over TCP and over one bidirectional QUIC stream; the QUIC transport half-closes the stream with `send_fin` and lingers up to 1 s on close so retained chunks are acknowledged before `CONNECTION_CLOSE`.

## Resume

Interrupted transfers continue from the last verified chunk. Progress is persisted in `.{filename}.embr.partial` alongside the output file:
```
[file_hash: 32 bytes][bitmap: ceil(chunk_count/8) bytes, LSB = chunk 0]
```

On restart, pull loads the bitmap, requests only missing chunks, verifies SHA256 per chunk before marking done. Partial file removed only when `all_done()`. Delete `.{filename}.embr.partial` manually to force a fresh transfer.

## Tracker

The tracker is a lightweight HTTP server mapping content-derived tokens to sender addresses. Token = `SHA256(concat(chunk_hashes))`, first 8 bytes hex-encoded — deterministic from file content, same file always produces same token.

```
POST /register          {token, sender_port} → 200 {sender_ip_recorded}
GET  /resolve/:token    → 200 {sender_ip, sender_port} or 404
POST /unregister/:token → 204
```

Tracker stores no file data — pure `token → (ip, port)` indirection. File integrity guaranteed end-to-end by SHA256 chunk hashes in `FILE_META`. Tokens expire after 10 minutes (configurable with `--ttl`).

## Roadmap

| Phase | What |
|-------|------|
| v0.1 | TCP whole-file transfer, pluggable transport, wire protocol ✓ |
| v0.2 | 1MB chunking + SHA256 per-chunk integrity ✓ |
| v0.3 | TCP + sendfile() + mmap(MAP_SHARED), zero-copy push ✓ |
| v0.4 | UDP + io_uring, registered buffers, direct-to-disk recv ✓ |
| v0.5 | Request-driven protocol, resume interrupted transfers ✓ |
| v0.6 | Token + tracker, splice() zero-copy recv, .embr.hash cache, embr trust ✓ |
| v0.7 | TCP path hardening, parallel pre-hash, strace zero-copy figure, WAN bench ✓ |
| **v0.8** | **QUIC transport (ngtcp2 + wolfSSL), ownership until ACK, io_uring SEND_ZC egress with fallbacks, QUIC WAN bench ✓** |
| v0.9 | Receiver batching (`recvmmsg`, hash off the connection thread), UDP GSO on the sender, server-cert verification, tracker over QUIC |
| v1.x | SPMC ring buffer, 1-to-N fanout, parallel chunks via QUIC streams, eBPF/XDP, multi-tracker federation |

## Current Status

**v0.8 — QUIC transport and SEND_ZC egress**

- [x] `QuicTransport` over ngtcp2 1.22 + wolfSSL 5.9 (`--enable-aesgcm-stream`, `--enable-quic`); `quic_connect` / `quic_listen` / `quic_accept` factories; `--transport quic` on push and pull
- [x] `send_file` maps the chunk and retains it in `unacked_` until the acknowledgement callback; `recv_file` writes into a mapped output region
- [x] `send_fin` half-close; graceful close lingers up to 1 s so retained chunks are acknowledged
- [x] `ZcEgress` — per-connection packet slab registered with io_uring, `IORING_OP_SEND_ZC` with usage reporting, slot owner is a `Buffer` retired on the NOTIF completion; send CQE with `F_MORE` waits, without it the slot is final
- [x] Egress fallbacks — refused datagram copy-sent once; no registration → sendmsg for the connection; pre-6.2 kernel → sendmsg; partial submit resubmitted from ring state; slab leaked, never reused, if the NIC may still hold it at teardown
- [x] `EMBR_QUIC_EGRESS=sendmsg` baseline switch; `EMBR_QUIC_STATS=1` per-connection egress counters
- [x] Load gate — 96 concurrent runs of the QUIC suite (12 × 8) pass in both egress modes; ASan/UBSan clean on the suite and a 1 GiB transfer
- [x] Memlock footprint measured — ~105 KiB per QUIC connection; fits the default 8 MiB
- [x] `bench/setup_quic_deps.sh`, `bench/quic_loopback.sh`, `bench/quic_wan.sh` — SEND_ZC vs sendmsg rows, sender CPU per push, copied ratio, kernel and memlock preflights
- [x] QUIC WAN benchmark — c5n.large cross-region, n=10: 0.91 Gbps receiver-bound, 0 of 11.2 M datagrams copied
- [x] `bench/erasure_cost.cpp` — type-erased release costs 11.5 ns (laptop) / 19.5 ns (c5n) per buffer, 0 allocations
- [x] `push.cpp` / `pull.cpp` unchanged through the QUIC and SEND_ZC transitions
- [ ] Server-certificate verification (client runs `SSL_VERIFY_NONE`)
- [ ] Receiver batching and sender GSO (validated on a scratch build, held for v0.9)

**v0.7 — TCP path hardening**

- [x] Pluggable `Transport` interface — control plane `send`/`recv`, data plane `send_file`/`recv_file`
- [x] `TcpTransport` + `tcp_connect` / `tcp_listen` / `tcp_accept` / `tcp_from_fd` factories
- [x] `TcpTransport::send_file` — `sendfile()` syscall, 0 copies push
- [x] `TcpTransport::recv_file` — `splice()` socket→pipe→file, 0 copies pull; pipe lazy-init, reused across chunks
- [x] `UdpTransport` — io_uring registered buffers, READ_FIXED + sendmsg, RECV + WRITE_FIXED direct-to-disk (experimental)
- [x] TCP socket tuning — `TCP_NODELAY` always on; `SO_SNDBUF`/`SO_RCVBUF` OS-autotuned (not pinned)
- [x] `SIGPIPE` ignored process-wide; `sendfile()` / `splice()` EINTR retry
- [x] `SocketFd` RAII wrapper; `util/exact_io.hpp`; `util/constants.hpp`
- [x] Custom binary wire protocol (`protocol.hpp/.cpp`), `PROTOCOL_VERSION=0x02`
- [x] `Buffer` — move-only, unified heap/mmap/slot backing via `std::function` release callback
- [x] `hash.hpp/.cpp` — SHA256 via OpenSSL EVP, parallel pre-hash (`hash_compute_parallel`), `.embr.hash` cache
- [x] Pre-committed chunk hashes in `FILE_META`; `ChunkManager` bitmap; `PartialFile` resume; `ftruncate` pre-allocation
- [x] Request-driven protocol — pull sends `CHUNK_REQ` per needed chunk + `COMPLETE`; push answers, stateless
- [x] Token derivation, `TokenStore` with TTL, tracker HTTP server and client, `embr trust`
- [x] Subcommand dispatch and full argparse per verb, `EMBR_TRACKER` env var
- [x] `bench/tcp_loopback.sh`, `bench/tcp_wan.sh`; strace zero-copy figure
- [x] Protocol, transport, resume, tracker and parallel-hash unit tests (82 tests at v0.8)

## License

[Mozilla Public License 2.0](https://www.mozilla.org/en-US/MPL/2.0/) — Modify embr's files → your changes must be open source. Use embr in your own project → your new files can be any license.
