# embr
A large-file transfer engine built with C++20: `sendfile` + `splice` on TCP, an encrypted QUIC transport (ngtcp2 + wolfSSL, TLS 1.3), and an optional io_uring `SEND_ZC` packet sender. Backends implement file-transfer operations and manage the memory lifetimes they require.

*From Old English ǣmyrġe, "smoldering ash." A shared file is like an ember: still glowing, passed from hand to hand, never fully extinguished.*

## Why

Moving large research datasets motivated embr. The project explores the CPU cost of file transfer and how its implementation changes when transport and encryption requirements change.

The TCP file-transfer path uses kernel I/O mechanisms. QUIC retains heap messages and mapped file chunks, and its `SEND_ZC` path also retains packet-pool slots. These resources use the same owning `Buffer` value with different release conditions. Further overlap between receiving chunks and asynchronous file writes is planned; the measurements below describe the implemented paths.

## Two Transfer Modes

**Where plaintext transfer is acceptable** — TCP with kernel file-transfer I/O:
- `sendfile()` on push and `splice()` on pull avoid application payload-buffer copies on the file-transfer path; integrity checking still reads the data
- `TCP_NODELAY` on control messages, `SO_SNDBUF`/`SO_RCVBUF` OS-autotuned

**Encrypted QUIC prototype** — ngtcp2 + wolfSSL:
- TLS 1.3 handshake, 30 s idle timeout, ICMP soft errors tolerated
- Chunks are `mmap`'d and retained for acknowledgement and retransmission; QUIC assembles and encrypts separate outgoing packets
- Datagrams leave through an io_uring `SEND_ZC` egress: a registered per-connection slab, each packet slot retained until the kernel permits its memory to be reused
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
  inflight_[i] = move(packet); SEND_ZC submit      retained until the kernel permits memory reuse
  ACK callback:  unacked_.pop_front()  → munmap
  NOTIF CQE:     inflight_[i] = Buffer{} → slot back to the free list
```
File-transfer calls pass a file descriptor, offset and length through `send_file` / `recv_file`; the backend selects the payload representation. Heap control-message Buffers and mapped file Buffers share `unacked_`; packet-slot Buffers use a separate `inflight_` table. A SEND_ZC send completion with `F_MORE` requires waiting for `NOTIF`; without `F_MORE`, that send completion is final. `Buffer` stores cleanup responsibility, while the backend decides when to retire it.

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

See [bench/bench.md](bench/bench.md) for methodology, consolidated raw data and reproduction commands. The WAN table is from the 2026-09-06 v0.8 campaign: two `c5n.large` instances, Amazon Linux 2023 kernel 6.18.44, us-east-1 → us-east-2, a 1 GiB file, file caches dropped on both ends, two warmups and ten timed rounds, with output SHA-256 verification. The syscall counts are from an older v0.7 run. The corrected desktop ownership benchmark is a separate measurement described below.

### TCP mechanism (v0.7 strace syscall count, 1 GiB)

| process | dominant syscalls | total syscalls |
|---------|-------------------|----------------|
| embr push | `sendfile` ×1024 | ~7,408 |
| embr pull | `splice` ×4096 + `mmap` ×1024 (SHA-256 verify) | ~15,532 |
| ncat sender | `read` ×131,072 + `sendto` ×131,072 + `fcntl` ×524,288* | ~917,656 |
| ncat receiver | `recvfrom` ×131,072 + `write` ×131,072 | ~393,430 |

\* ncat artifact, not protocol work.

**The TCP file-transfer path avoids application payload buffers.** `sendfile` and `splice` transfer file data through the kernel; ncat uses a userspace buffer. Integrity hashing reads the data separately. These historical syscall counts illustrate the I/O path; they are not a measurement of every memory copy or of `Buffer` overhead. Timings come from untraced runs.

### WAN cross-region (1 GiB, AWS c5n.large us-east-1 → us-east-2, n=10, medians)

| tool | throughput | wall | receiver user / sys | sender user / sys |
|------|-----------|------|---------------------|-------------------|
| embr TCP (sendfile / splice) | 2.005 Gbps | 4.29 s | 2.64 s / 0.72 s | 0.00 s / 0.32 s |
| nc | 2.225 Gbps | 3.87 s | 0.27 s / 1.06 s | — |
| scp† | 1.115 Gbps | 7.73 s | 0.96 s / 1.94 s | — |
| embr QUIC, `SEND_ZC` egress | 0.910 Gbps | 9.41 s | 5.74 s / 3.47 s | 2.77 s / 2.97 s |
| embr QUIC, `sendmsg` egress | 0.905 Gbps | 9.51 s | 5.81 s / 3.59 s | 2.94 s / 2.56 s |

†scp is an encrypted reference with different protocol and processing work. The first two successful transfers in each row were warmups; interrupted or failed sender processes are excluded. CPU columns are independently calculated medians, not components of a median total.

**TCP path.** Throughput ranges overlap (embr 1.81–2.19 Gbps, nc 2.00–2.74), while nc's median is about 11% higher. Receiver kernel CPU is about 33% lower for embr using the unrounded medians (0.715 s vs 1.060 s); receiver user CPU is higher (2.64 s vs 0.27 s). embr performs per-chunk verification during the timed transfer. These workloads differ, and the totals do not isolate copy or hashing cost.

**QUIC path.** The two sending modes have similar throughput in this experiment. For SEND_ZC, median receiver CPU (user + sys computed for each run) is 9.23 s per GiB against 9.41 s elapsed, consistent with a CPU-limited receiver. These process-level timings do not separate hashing, decryption, packet handling and memory-copy costs. Receive batching and overlapping receive with file writes are future work.

**`SEND_ZC` result.** The archived terminal summary reports **0 copied among 11,243,123 sends**, with no fallbacks or errors, over 12 connections including warmups. Individual egress logs were not preserved, so this aggregate cannot be independently recomputed from the retained files. Median sender total CPU, computed per successful timed push, is 5.755 s with SEND_ZC and 5.565 s with sendmsg. SEND_ZC did not reduce sender CPU in this campaign. Completion processing and packet size are candidates for further study; segmentation offload has no demonstrated benefit in these data.

### Desktop ownership benchmark

The corrected [erasure_cost.cpp](bench/erasure_cost.cpp) compares the existing `Buffer` with a move-only function-pointer owner carrying inline pool/slot state. Both construct, move into a table, expose the retained owner to a compiler barrier, retire it and return its slot. Lifetime checks precede timing.

| Representation | Median ns/lifetime | Min–max ns/lifetime | Object size |
|---|---:|---:|---:|
| `Buffer` / `std::function` | 15.552 | 15.366–17.313 | 56 B |
| `RawBuf` / function pointer | 1.649 | 1.635–1.660 | 40 B |

AMD Ryzen 9 9900X desktop, Fedora Linux kernel 7.1.13, GCC 16.2.1 / libstdc++ 20260819, `-O3 -DNDEBUG`; ten alternating-order rounds of 20 million lifetimes per owner, after one million warmup lifetimes each. Both rows reported zero intercepted ordinary scalar `operator new` calls. The paired-difference median is 13.900 ns/lifetime. These are complete owner-loop measurements, including barrier and cleanup work; they do not establish isolated type-erasure cost or a production-overhead percentage. The earlier desktop/c5n cost estimates are superseded; the corrected benchmark has only been measured on this desktop. [Raw output and reproduction](bench/bench.md#desktop-ownership-cost).

## Build

Install the dependencies below, then run from the repository root. The current CMake configuration requires the QUIC libraries even for TCP-only use.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/embr_test
```

Run the test executable directly: [CMakeLists.txt](CMakeLists.txt) does not currently enable top-level CTest discovery. `ctest --test-dir build` therefore does not run this suite. See [CMake's enable_testing requirement](https://cmake.org/cmake/help/latest/command/enable_testing.html).

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
│   │   ├── push.hpp/.cpp               # sender logic, metadata precomputation
│   │   └── pull.hpp/.cpp               # receiver logic, resume
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
│   └── erasure_cost.cpp                # whole-owner lifecycle checks and desktop microbenchmark
├── tests/
│   ├── test_protocol.cpp, test_tcp.cpp, test_udp.cpp, test_resume.cpp, test_tracker.cpp
│   ├── test_hash_parallel.cpp          # parallel vs serial SHA-256 correctness
│   ├── test_quic.cpp                   # handshake, echo, send_file/recv_file, ACK retention, egress path
│   └── test_egress.cpp                 # ZcEgress slot lifecycle, drain, destructor
└── CMakeLists.txt
```

Business logic (`push`, `pull`) talks only to `Transport&` — never to raw sockets. Transport lifecycle is owned by the CLI layer. The `run_push` / `run_pull` function bodies are byte-identical at v0.5, v0.6, v0.7 and v0.8 (`9342d1e`). The splice receiver (v0.6), QUIC transport (v0.8) and `SEND_ZC` egress (v0.8) each landed without touching `push.cpp` or `pull.cpp`.

Separate changes to `push.cpp` added hash caching and parallel metadata precomputation. The earlier io_uring UDP experiment needed a second `Transport&` for its data plane and was withdrawn from the CLI. [Recorded source comparisons](POSTER-REVISION.md).

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

Pull saves completed-chunk flags in `.{filename}.embr.partial` alongside the output file. When it accepts that saved progress, it requests only chunks still marked incomplete:

```
[first_chunk_sha256: 32 bytes][bitmap: ceil(chunk_count/8) bytes, LSB = chunk 0]
```

Newly received chunks are checked against their SHA-256 hashes before being marked done. The partial record is removed when `all_done()` is true. Delete `.{filename}.embr.partial` manually to force a fresh transfer.

Current resume limitations:

- [run_pull](src/core/pull.cpp) stores `file_meta.chunk_hashes[0]`, which identifies only the first chunk. Saved completed chunks are not rehashed on restart. Reusing an output path for another file sharing the first chunk and chunk count can accept stale progress.
- [PartialFile::load](src/core/partial_file.cpp) requires the output size to equal `chunk_count * CHUNK_SIZE`. With 1 MiB chunks, a file containing a partial last chunk restarts instead of resuming.

These are implementation limitations; the current format does not validate whole-file identity or detect changes to previously completed chunks.

## Tracker

The tracker is a lightweight HTTP server mapping content-derived tokens to sender addresses. Token = `SHA256(concat(chunk_hashes))`, first 8 bytes hex-encoded — deterministic from file content, same file always produces same token.

```
POST /register          {token, sender_port} → 200 {sender_ip_recorded}
GET  /resolve/:token    → 200 {sender_ip, sender_port} or 404
POST /unregister/:token → 204
```

Tracker stores no file data — pure `token → (ip, port)` indirection. Newly received chunks are checked against the SHA-256 values supplied in `FILE_META`; those hashes do not authenticate the sender. Saved progress has the [resume limitations](#resume) above. Tokens expire after 10 minutes (configurable with `--ttl`).

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
- [x] QUIC WAN benchmark — c5n.large cross-region, n=10: 0.910 Gbps; archived terminal summary reports no copied sends (individual egress logs unavailable)
- [x] Corrected desktop ownership benchmark — `Buffer` 15.552 ns/lifetime vs `RawBuf` 1.649 ns/lifetime; ten rounds, active lifetime checks, scoped allocation counts and raw output in `bench/bench.md`
- [x] `push.cpp` / `pull.cpp` unchanged through the QUIC and SEND_ZC transitions
- [ ] Server-certificate verification (client runs `SSL_VERIFY_NONE`)
- [ ] Receive batching, asynchronous file-write overlap and sender segmentation offload; validate performance before making benefit claims

**v0.7 — TCP path hardening**

- [x] Pluggable `Transport` interface — control plane `send`/`recv`, data plane `send_file`/`recv_file`
- [x] `TcpTransport` + `tcp_connect` / `tcp_listen` / `tcp_accept` / `tcp_from_fd` factories
- [x] `TcpTransport::send_file` — `sendfile()` push avoids application payload-buffer copies
- [x] `TcpTransport::recv_file` — `splice()` socket→pipe→file avoids application payload-buffer copies; pipe lazy-init, reused across chunks
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
