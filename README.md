# embr
A zero-copy large file transfer engine built with C++20, io_uring, and a pluggable transport layer.

*From Old English ǣmyrġe, "smoldering ash." A shared file is like an ember: still glowing, passed from hand to hand, never fully extinguished.*

## Why

Existing tools arrange disk I/O and network I/O sequentially — the disk waits for the network, the network waits for the disk. Add redundant memory copies between kernel and userspace, and even 10G links stay half-idle.

embr pipelines disk and network operations in parallel and eliminates intermediate copies, targeting near-line-rate throughput at minimal CPU overhead.

## Two Transfer Modes

**Trusted network (LAN / datacenter)** — custom UDP reliability layer + io_uring:
- No TLS overhead, io_uring registered buffers, self-managed reliability (seq/ACK/retransmit)
- Target: saturate 10G+ links at <30% single-core CPU
- TCP available as fallback for restricted networks

**Public P2P** — ngtcp2 + io_uring:
- TLS 1.3, NAT traversal, token-based peer discovery via tracker
- Same io_uring buffer infrastructure, ngtcp2 QUIC state machine on top
- TCP available as fallback where QUIC is blocked

## Usage
```bash
# v0.4 — zero-copy TCP + UDP + io_uring (current)
embr push <file> [--port PORT] [--tcp]
embr pull <ip>   [--port PORT] [--out PATH] [--tcp]

# Default: TCP control channel (port) + UDP data plane (port+1)
# --tcp: single TCP connection for both control and data

# v0.6+ — tracker mode (planned)
embr push large_dataset.tar.gz   # → token: Kf3xQ9mZ
embr pull Kf3xQ9mZ               # resolves via tracker
embr pull Kf3xQ9mZ 192.168.1.50  # direct mode, skip tracker
```

## How It Works

**Direct mode (v0.4 — current)**
```
Sender                                                     Receiver
  │  embr push file.tar.gz                                     │
  │  → listening on :9000 (TCP control) + :9001 (UDP data)     │
  │  ←──────────── HANDSHAKE ──────────────────── [TCP] ─────  │
  │  ─── FILE_META {filename, size, chunk_hashes[0..N]} ─────→ │
  │  ←──────────── ready byte ─────────────────── [TCP] ─────  │
  │  ──── fragments (io_uring sendmsg, 0 copies) ── [UDP] ───→ │
  │  ←──────────── ACK{chunk_index} ──────────── [TCP] ─────   │
  │  ...                                                       │
  │  ←──────────── COMPLETE ───────────────────── [TCP] ─────  │
```

Push pre-computes SHA256 for all chunks before transfer and commits them in `FILE_META`.
Pull verifies all chunks after transfer against pre-committed hashes.
UDP data plane uses io_uring registered buffers — disk reads via READ_FIXED, network sends via sendmsg scatter-gather, network receives via RECV + WRITE_FIXED direct-to-disk.
TCP fallback uses `sendfile()` on push (0 copies) and `mmap(MAP_SHARED)` on pull (1 copy).

**Tracker mode (v0.6+ — planned)**
```
Sender                        Tracker                      Receiver
  │──── POST /register ───────>│                              │
  │<─── token: Kf3xQ9mZ ───────│                              │
  │                            │<──── GET /resolve/Kf3xQ9mZ ─ │
  │                            │───── {addr, size} ──────────>│
  │<══════════ UDP + parallel chunk transfer ════════════════>│
```

## Benchmark

Localhost, 3GB file (Fedora ISO), `MTU=65000`:

| Tool | Wall | User | Sys | Throughput |
|------|------|------|-----|------------|
| embr UDP + io_uring | 2.113s | 1.142s | 0.775s | 1.22 GB/s |
| embr TCP + sendfile | 1.851s | 1.092s | 0.709s | 1.39 GB/s |
| scp | 1.064s | 0.295s | 0.936s | 2.28 GB/s |

Remote VPS, 3GB file, ~25 MB/s uplink:

| Tool | Wall | User | Sys | Throughput |
|------|------|------|-----|------------|
| embr TCP + sendfile | 131.2s | 2.0s | 8.9s | 23.4 MB/s |
| scp | 117.7s | 3.6s | 12.5s | 26.1 MB/s |

embr TCP uses 44% less userspace CPU than scp on the remote path (2.0s vs 3.6s user) — SHA256 is pre-computed before transfer begins, not in the hot path.

UDP sys time matches TCP (0.775s vs 0.709s) — io_uring kernel overhead eliminated at large MTU. Remaining wall-time gap vs TCP = stop-and-wait ACK latency (2616 chunks × TCP RTT). Sliding window is deferred to the ngtcp2 phase where QUIC streams provide it for free. Gap vs scp = SHA256 chunk verification cost (user time). scp performs no integrity verification.

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

**macOS (development only — io_uring not supported, TCP path only):**
```bash
brew install cmake ninja openssl
```

**Kernel requirement:** Linux 6.0+ for io_uring RECV/WRITE_FIXED on UDP sockets. Older kernels fall back to unregistered buffers automatically.

**memlock limit:** io_uring registered buffers require sufficient locked memory. Default 8MB is sufficient for `CHUNK_SIZE=1MB` (2MB chunk pool + 2MB frag pool). Verify with `ulimit -l`.

## Benchmark

Localhost, 3GB file (Fedora ISO), `MTU=65000`:

| Tool | Wall | User | Sys | Throughput |
|------|------|------|-----|------------|
| embr UDP + io_uring | 2.113s | 1.142s | 0.775s | 1.22 GB/s |
| embr TCP + sendfile | 1.851s | 1.092s | 0.709s | 1.39 GB/s |
| scp | 1.064s | 0.295s | 0.936s | 2.28 GB/s |

Remote VPS, 3GB file, ~25 MB/s uplink:

| Tool | Wall | User | Sys | Throughput |
|------|------|------|-----|------------|
| embr TCP + sendfile | 131.2s | 2.0s | 8.9s | 23.4 MB/s |
| scp | 117.7s | 3.6s | 12.5s | 26.1 MB/s |

embr TCP uses 44% less userspace CPU than scp on the remote path (2.0s vs 3.6s user) — SHA256 is pre-computed before transfer begins, not in the hot path.

UDP sys time matches TCP (0.775s vs 0.709s) — io_uring kernel overhead eliminated at large MTU. Remaining wall-time gap vs TCP = stop-and-wait ACK latency (2616 chunks × TCP RTT). Sliding window is deferred to the ngtcp2 phase where QUIC streams provide it for free. Gap vs scp = SHA256 chunk verification cost (user time). scp performs no integrity verification.
### Dependencies

- C++20 compiler (GCC 12+ / Clang 15+)
- CMake 3.25+
- OpenSSL (SHA256 via EVP interface)
- liburing (io_uring support)
- GoogleTest (fetched via CMake FetchContent)

## Architecture
```
embr/
├── src/
│   ├── main.cpp                        # CLI routing, transport lifecycle
│   ├── core/
│   │   ├── protocol.hpp/.cpp           # send_msg/recv_msg, wire format
│   │   ├── chunk_manager.hpp           # chunk state bitmap
│   │   ├── hash.hpp/.cpp               # SHA256 via OpenSSL EVP
│   │   ├── push.hpp/.cpp               # sharer logic
│   │   └── pull.hpp/.cpp               # fetcher logic
│   ├── transport/
│   │   ├── transport.hpp               # abstract interface
│   │   ├── tcp_transport.hpp/.cpp      # TCP implementation
│   │   ├── tcp_client.hpp/.cpp         # tcp_connect() factory
│   │   ├── tcp_server.hpp/.cpp         # tcp_listen() / tcp_accept() factories
│   │   ├── udp_transport.hpp/.cpp      # UDP + io_uring implementation
│   │   ├── udp_bind.hpp/.cpp           # udp_data_server_bind/connect factories
│   │   └── udp_connect.hpp/.cpp        # udp_data_client_connect factory
│   └── util/
│       ├── socket_fd.hpp               # RAII fd wrapper
│       ├── io.hpp                      # send_exact / recv_exact
│       ├── io_uring_ctx.hpp/.cpp       # io_uring ring + registered buffer pool
│       └── constants.hpp              # CHUNK_SIZE, UDP_MTU, fragment geometry
├── tests/
│   ├── test_protocol.cpp
│   ├── test_tcp.cpp
│   └── test_udp.cpp
└── CMakeLists.txt
```

Business logic (`push`, `pull`) talks only to `Transport&` — never to raw sockets.
Transport lifecycle owned by `main.cpp`. Swapping transport = one file change.

## Wire Protocol
```
Header (6 bytes, always): [version:u8][type:u8][payload_len:u32 BE]

Message types:
  HANDSHAKE  (0x01)  [token_len:u32 BE][token:utf8]
  FILE_META  (0x02)  [file_size:u64 BE][filename_len:u32 BE][filename:utf8]
                     [chunk_size:u32 BE][chunk_count:u32 BE]
                     [chunk_hash[0]:32B]...[chunk_hash[N-1]:32B]
  CHUNK_REQ  (0x03)  [chunk_index:u32 BE]
  CHUNK_HDR  (0x04)  [chunk_index:u32 BE] + raw bytes via data plane
  COMPLETE   (0x06)  (no payload)
  ERROR      (0x07)  [reason:utf8]
  CANCEL     (0x08)  (no payload)
```

UDP fragment format (10 bytes, prepended to each datagram):
```
[chunk_index:u32 BE][frag_index:u32 BE][frag_len:u16 BE]
```

## Roadmap

| Phase | What |
|-------|------|
| v0.1 | TCP whole-file transfer, pluggable transport, wire protocol ✓ |
| v0.2 | 16MB chunking + SHA256 per-chunk integrity ✓ |
| v0.3 | TCP + sendfile() + mmap(MAP_SHARED), zero-copy data plane ✓ |
| **v0.4** | **UDP + io_uring, registered buffers, direct-to-disk recv, dual-channel (TCP control + UDP data) ✓** |
| v0.5 | Resume interrupted transfers — .embr.partial bitmap, CHUNK_REQ selective resend |
| v0.6 | Token + tracker HTTP server, embr push → token, embr pull \<token\> |
| v0.7+ | ngtcp2 + io_uring, public P2P, TLS 1.3, NAT traversal, sliding window, 1-to-N fanout |
| v1.x | ngtcp2 + eBPF/XDP, AF_XDP bypass, multi-seeder |

## Current Status

**v0.4 — UDP + io_uring data plane, TCP control channel**

- [x] Project skeleton, CMake, GoogleTest
- [x] Pluggable `Transport` interface — control plane `send`/`recv`, data plane `send_file`/`recv_file`
- [x] `TcpTransport` + `tcp_connect` / `tcp_listen` / `tcp_accept` factories
- [x] `TcpTransport::send_file` — `sendfile()` syscall, 0 copies push
- [x] `TcpTransport::recv_file` — `mmap(MAP_SHARED)` + `recv_exact`, 1 copy pull
- [x] `UdpTransport` — io_uring registered buffers, READ_FIXED + sendmsg scatter-gather, RECV + WRITE_FIXED direct-to-disk
- [x] Dual-channel: TCP for HANDSHAKE / FILE_META / ACK / COMPLETE, UDP for file data
- [x] Double-buffer pipeline: disk read of chunk N+1 overlaps network send of chunk N
- [x] Per-chunk ACK over TCP: stop-and-wait, whole-chunk retransmit on loss
- [x] IoUringCtx: single `io_uring_register_buffers` call, ENOMEM fallback to unregistered
- [x] UDP peer discovery: probe-based (`udp_data_server_bind/connect` + `udp_data_client_connect`)
- [x] `SocketFd` RAII wrapper
- [x] `util/io.hpp` — `send_exact` / `recv_exact`
- [x] `util/constants.hpp` — `CHUNK_SIZE=1MB`, `UDP_MTU`, fragment geometry
- [x] Custom binary wire protocol (`protocol.hpp/.cpp`), `PROTOCOL_VERSION=0x02`
- [x] `Buffer` — move-only, unified heap/mmap/io_uring backing via `std::function` release callback
- [x] `ChunkManager` — `vector<bool>` bitmap, bounds-checked, parallel-ready interface
- [x] `hash.hpp/.cpp` — SHA256 via OpenSSL EVP, `EvpCtx` RAII wrapper
- [x] Pre-committed chunk hashes — all SHA256s computed before transfer, embedded in `FILE_META`
- [x] `ftruncate` pre-allocation — `pwrite` at arbitrary offsets, parallel-ready
- [x] CLI: `embr push <file> [--tcp]` / `embr pull <ip> [--out PATH] [--tcp]`
- [x] Protocol unit tests — round-trips, malicious chunk_count guard, Buffer ownership
- [x] Transport unit tests — echo, aligned/unaligned send_file/recv_file, UDP multi-chunk

## License

[Mozilla Public License 2.0](https://www.mozilla.org/en-US/MPL/2.0/) — Modify embr's files → your changes must be open source. Use embr in your own project → your new files can be any license.