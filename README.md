# embr
A zero-copy large file transfer engine built with C++20, zero-copy I/O (sendfile + splice / io_uring), and a pluggable transport layer.

*From Old English ǣmyrġe, "smoldering ash." A shared file is like an ember: still glowing, passed from hand to hand, never fully extinguished.*

## Why

Existing tools arrange disk I/O and network I/O sequentially — the disk waits for the network, the network waits for the disk. Add redundant memory copies between kernel and userspace, and even 10G links stay half-idle.

embr pipelines disk and network operations in parallel and eliminates intermediate copies, targeting near-line-rate throughput at minimal CPU overhead.

## Two Transfer Modes

**Trusted network (LAN / datacenter)** — TCP with zero-copy I/O:
- `sendfile()` on push (0 copies), `splice()` on pull (0 copies)
- `TCP_NODELAY` on control messages, `SO_SNDBUF`/`SO_RCVBUF` OS-autotuned
- io_uring UDP data plane available (benchmarked, closed pending single-transport refactor)

**Public network (P2P / discovery)** — ngtcp2 + io_uring (planned):
- TLS 1.3, NAT traversal, token-based peer discovery via tracker
- Same io_uring buffer infrastructure, ngtcp2 QUIC state machine on top

## Usage

```bash
# Direct mode — push listens, pull connects by IP
embr push <file> [--port PORT]
embr pull <ip>   [--port PORT] [--out PATH]

# Tracker mode — push registers a token, pull resolves it
embr push <file> --tracker http://104.194.95.77:10009
embr pull <token> --tracker http://104.194.95.77:10009

# Trust a tracker — persists URL, no --tracker flag needed after
embr trust http://104.194.95.77:10009
embr push <file>     # auto-registers token
embr pull <token>    # auto-resolves via trusted tracker

# Run your own tracker
embr tracker [--bind ADDR] [--port PORT] [--ttl MINUTES]

# Manage trusted tracker
embr trust --show    # print current trusted tracker
embr trust --clear   # remove saved tracker
```

## How It Works

**v0.6 — token + tracker**
```
Sender                        Tracker (plurb.org:10009)     Receiver
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

See [BENCHMARKS.md](BENCHMARKS.md) for full methodology, raw data, and reproduction commands.

### Zero-copy mechanism (strace syscall count, 1 GB loopback)

| process | dominant syscalls | total syscalls |
|---------|-------------------|----------------|
| embr push | `sendfile` ×1024 | ~7,408 |
| embr pull | `splice` ×4096 + `mmap` ×1024 (SHA-256 verify) | ~15,532 |
| ncat sender | `read` ×131,072 + `sendto` ×131,072 + `fcntl` ×524,288* | ~917,656 |
| ncat receiver | `recvfrom` ×131,072 + `write` ×131,072 | ~393,430 |

\* ncat artifact, not protocol work.

**Bytes through userspace: embr = 0. ncat = 1 GB.** sendfile/splice move pages entirely in-kernel; ncat copies every byte through an 8 KB userspace buffer (131,072 iterations per side).

### WAN cross-region (1 GB, AWS c5n.large us-east-1 → us-east-2, n=10)

| tool | throughput | wall (median) | sys (median) |
|------|-----------|--------------|-------------|
| embr | 1.17 Gbps | 7.33s | 0.82s |
| nc   | 1.17 Gbps | 7.33s | 0.945s |
| scp† | 0.83 Gbps | 10.41s | 2.50s |

†scp is SSH-encrypted — reference only, not an I/O comparison.

embr matches raw netcat throughput while verifying end-to-end integrity, at **13–15% lower kernel CPU** (no overlap across all 10 runs: embr worst sys 0.88s < nc best 0.90s). embr user time (2.58s) is receiver-side SHA-256 verification — work nc does not do. The zero-copy advantage becomes more dramatic on a fat, low-RTT link where copy/CPU is the bottleneck, not the network.

### Loopback software overhead (1 GB, Fedora laptop, n=5)

| tool | throughput | send_sys | total CPU |
|------|-----------|----------|-----------|
| embr | 15.34 Gbps | 0.04s | 0.58s |
| nc   | 20.45 Gbps | 0.27s | 0.67s |
| scp† | 8.77 Gbps  | 0.00s | 0.98s |

nc wins loopback throughput — no protocol overhead, no integrity verification. The signal is **send_sys: embr 0.04s vs nc 0.27s — 6.75× lower sender kernel time**. sendfile hands pages to the socket buffer in-kernel; nc's sender calls read() + send() per chunk. embr uses less total CPU (0.58s) than nc (0.67s) despite verifying every chunk with SHA-256.

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

**Kernel requirement:** Linux 6.0+ for io_uring RECV/WRITE_FIXED on UDP sockets. Older kernels fall back to unregistered buffers automatically.

**memlock limit:** io_uring registered buffers require sufficient locked memory. Default 8MB is sufficient for `CHUNK_SIZE=1MB`. Verify with `ulimit -l`.

## Architecture
```
embr/
├── src/
│   ├── main.cpp                        # verb dispatch (~20 lines)
│   ├── cli/
│   │   ├── push_cli.hpp/.cpp           # push argparse, token derivation, tracker register
│   │   ├── pull_cli.hpp/.cpp           # pull argparse, token detection, tracker resolve
│   │   ├── tracker_cli.hpp/.cpp        # tracker argparse, run_tracker_server
│   │   ├── trust_cli.hpp/.cpp          # trust argparse, ~/.config/embr/tracker
│   │   └── bench.hpp/.cpp              # bench argparse, --role sender/receiver
│   ├── core/
│   │   ├── protocol.hpp/.cpp           # send_msg/recv_msg, wire format
│   │   ├── chunk_manager.hpp/.cpp      # runtime bitmap of completed chunks
│   │   ├── partial_file.hpp/.cpp       # .embr.partial serialize/deserialize
│   │   ├── hash.hpp/.cpp               # SHA256 + parallel pre-hash + .embr.hash cache
│   │   ├── push.hpp/.cpp               # sender logic
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
│   │   └── tcp_server.hpp/.cpp         # tcp_listen() / tcp_accept() / tcp_from_fd()
│   └── util/
│       ├── socket_fd.hpp               # RAII fd wrapper
│       ├── exact_io.hpp                # send_exact/recv_exact
│       ├── json_parser.hpp             # hand-rolled flat JSON helpers
│       ├── config_tracker.hpp          # resolve_tracker_url, read/write config
│       ├── io_uring_ctx.hpp/.cpp       # io_uring ring + registered buffer pool
│       └── constants.hpp              # CHUNK_SIZE, HASH_SIZE, EMBR_PORT, TRACKER_PORT
├── bench/
│   ├── bench.md                        # methodology notes
│   ├── tcp_loopback.sh                 # loopback benchmark (sender+receiver CPU)
│   └── tcp_wan.sh                      # WAN benchmark (interleaved, retry, verify)
├── tests/
│   ├── test_protocol.cpp
│   ├── test_tcp.cpp
│   ├── test_resume.cpp
│   ├── test_tracker.cpp
│   └── test_hash_parallel.cpp          # parallel vs serial SHA-256 correctness
└── CMakeLists.txt
```

Business logic (`push`, `pull`) talks only to `Transport&` — never to raw sockets. Transport lifecycle owned by CLI layer. Swapping transport = one file change. The Transport interface survived three generations of data-plane optimization (buffered I/O → sendfile+mmap → sendfile+splice) without touching `push.cpp` or `pull.cpp`.
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
| **v0.7** | **TCP path hardening, parallel pre-hash, strace zero-copy figure, WAN bench ✓** |
| v0.8 | ngtcp2 + io_uring QUIC transport, TLS 1.3, NAT traversal, persistent push daemon |
| v0.9 | SPMC lock-free ring buffer, 1-to-N fanout, parallel chunks via QUIC streams |
| v1.x | eBPF/XDP, AF_XDP bypass, multi-tracker federation |


## Current Status

**v0.7 — TCP path hardening**

- [x] Project skeleton, CMake, GoogleTest
- [x] Pluggable `Transport` interface — control plane `send`/`recv`, data plane `send_file`/`recv_file`
- [x] `TcpTransport` + `tcp_connect` / `tcp_listen` / `tcp_accept` / `tcp_from_fd` factories
- [x] `TcpTransport::send_file` — `sendfile()` syscall, 0 copies push
- [x] `TcpTransport::recv_file` — `splice()` socket→pipe→file, 0 copies pull; pipe lazy-init, reused across chunks
- [x] `UdpTransport` — io_uring registered buffers, READ_FIXED + sendmsg, RECV + WRITE_FIXED direct-to-disk
- [x] TCP socket tuning — `TCP_NODELAY` always on; `SO_SNDBUF`/`SO_RCVBUF` OS-autotuned (not pinned)
- [x] `SIGPIPE` ignored process-wide — `sendfile()` has no `MSG_NOSIGNAL` equivalent
- [x] `SPLICE_F_MORE` removed — no-op when destination is pipe or file
- [x] `sendfile()` / `splice()` EINTR retry — signal interrupt no longer aborts mid-chunk transfer
- [x] `tcp_from_fd` adopt factory — bench runner owns socket setup, wraps into TcpTransport
- [x] `SocketFd` RAII wrapper
- [x] `util/exact_io.hpp` — `send_exact`/`recv_exact`
- [x] `util/constants.hpp` — `CHUNK_SIZE=1MB`, `HASH_SIZE`, `EMBR_PORT=10007`, `TRACKER_PORT=10009`
- [x] Custom binary wire protocol (`protocol.hpp/.cpp`), `PROTOCOL_VERSION=0x03`
- [x] `Buffer` — move-only, unified heap/mmap/io_uring backing via `std::function` release callback
- [x] `hash.hpp/.cpp` — SHA256 via OpenSSL EVP, parallel pre-hash (`hash_compute_parallel`), `.embr.hash` cache
- [x] Parallel pre-hash — `alignas(64)` atomic work-stealing counter, `jthread` pool, whole-file mmap (avoids TLB-shootdown), exception propagation
- [x] Pre-committed chunk hashes — all SHA256s computed before transfer, embedded in `FILE_META`
- [x] `ChunkManager` — `vector<bool>` bitmap, bounds-checked
- [x] `PartialFile` — `.embr.partial` serialize/deserialize, hash + output file validation on load
- [x] Request-driven protocol — pull sends `CHUNK_REQ` per needed chunk + `COMPLETE`; push answers, stateless
- [x] Resume — mark-after-verify ordering; in-flight chunk at kill time stays unmarked, replays on restart
- [x] `ftruncate` pre-allocation — recv at arbitrary offsets, parallel-ready
- [x] Token derivation — `SHA256(concat(chunk_hashes))`, first 8 bytes hex, content-derived, deterministic
- [x] `TokenStore` — `unordered_map` + mutex + TTL eviction + background `jthread` sweeper
- [x] Tracker HTTP server — `POST /register`, `GET /resolve/:token`, `POST /unregister/:token`
- [x] Tracker client — `tracker_register`/`tracker_resolve`/`tracker_unregister` (noexcept unregister)
- [x] `embr trust` — persist tracker URL to `~/.config/embr/tracker`, auto-used by push/pull
- [x] Subcommand dispatch — `embr push` / `embr pull` / `embr tracker` / `embr trust`
- [x] CLI: full argparse per verb, `EMBR_TRACKER` env var, `--tracker` flag override
- [x] Bench harness — `bench/latency.hpp/.cpp`, `bench_cli`, HDR histogram, short/full loop
- [x] `bench/tcp_loopback.sh` — sender + receiver CPU measured separately, interleaved embr/nc/scp
- [x] `bench/tcp_wan.sh` — two-machine, retry+verify loop, interleaved, median summary
- [x] strace zero-copy figure — `sendfile×1024` + `splice×4096` vs ncat `read/write×131,072` each side
- [x] WAN benchmark — AWS c5n.large cross-region, 1 GB, n=10: embr matches nc throughput at 13–15% lower sys CPU
- [x] Loopback benchmark — embr send_sys 0.04s vs nc 0.27s (6.75× lower); embr total CPU lower despite SHA-256
- [x] Protocol unit tests, transport unit tests, resume unit tests, tracker unit tests, parallel hash tests
- [x] `BENCHMARKS.md` — strace table, WAN raw results, loopback raw results, methodology, reproduction commands

## License

[Mozilla Public License 2.0](https://www.mozilla.org/en-US/MPL/2.0/) — Modify embr's files → your changes must be open source. Use embr in your own project → your new files can be any license.