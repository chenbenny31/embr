### `README.md`

# embr

A high-performance peer-to-peer file transfer tool built with C++20, QUIC, and zero-copy I/O.

*From Old English ǣmyrġe, "smoldering ash." A shared file is like an ember: still glowing, passed from hand to hand, never fully extinguished.*

## Why

Existing tools arrange disk I/O and network I/O sequentially — the disk waits for the network, the network waits for the disk. Add redundant memory copies between kernel and userspace, and even 10G links stay half-idle.

embr pipelines disk and network operations in parallel and eliminates intermediate copies, targeting near-line-rate throughput at minimal CPU overhead.

## Usage

```bash
# Sender: share a file
embr push large_dataset.tar.gz

# Receiver: direct P2P
embr pull <ip> [--port PORT] [--out PATH]
```

## How It Works

Files are transferred over a direct TCP connection (v0.1). A custom binary protocol carries file metadata and raw file bytes over a pluggable `Transport` interface — swapping TCP for QUIC is a one-file change.

**v0.2+: tracker mode and chunking**

```
Sender                        Tracker                      Receiver
  │                              │                              │
  │──── POST /register ─────────>│                              │
  │<─── token: Kf3xQ9mZ ────────│                              │
  │                              │<──── GET /resolve/Kf3xQ9mZ ─│
  │                              │───── {addr, hash, size} ────>│
  │                              │                              │
  │<════════════ QUIC connect + parallel chunk transfer ═══════>│
```

**Direct mode (v0.1 — current)**

```
Sender                                                     Receiver
  │                                                            │
  │  embr push file.tar.gz                                     │
  │  → listening on :9000                                      │
  │                                    embr pull 192.168.1.50  │
  │                                                            │
  │<════════════ TCP connect + whole-file transfer ════════════>│
```

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

### Dependencies

- C++20 compiler (GCC 12+ / Clang 15+)
- CMake 3.25+
- GoogleTest (fetched via CMake FetchContent)

## Architecture

```
embr/
├── src/
│   ├── main.cpp                  # CLI routing, transport lifecycle
│   ├── core/
│   │   ├── protocol.hpp/.cpp     # send_msg/recv_msg, wire format
│   │   ├── push.hpp/.cpp         # sharer logic
│   │   └── pull.hpp/.cpp         # fetcher logic
│   ├── transport/
│   │   ├── transport.hpp         # abstract interface
│   │   ├── tcp_transport.hpp/.cpp # TCP send/recv implementation
│   │   ├── tcp_client.hpp/.cpp   # tcp_connect() factory
│   │   └── tcp_server.hpp/.cpp   # tcp_listen() / tcp_accept() factories
│   └── util/
│       └── socket_fd.hpp         # RAII fd wrapper
├── tests/
│   └── test_protocol.cpp
└── CMakeLists.txt
```

Business logic (`push`, `pull`) talks only to `Transport&` — never to raw sockets.
Transport lifecycle is owned by `main.cpp`. Swapping TCP → QUIC touches one file.

## Wire Protocol

```
Header (6 bytes): [version:u8][type:u8][payload_len:u32 BE]

Message types:
  FILE_META  (0x02)  [file_size:u64 BE][filename_len:u32 BE][filename:utf8]
  COMPLETE   (0x06)  (no payload)
  ERROR      (0x07)  [reason:utf8]
```

## Roadmap

| Phase | What |
|-------|------|
| **v0.1** | **TCP whole-file transfer, pluggable transport, wire protocol ✓** |
| v0.2 | 16MB chunking + SHA256, tracker, token-based discovery |
| v0.3 | QUIC transport (msquic), io_uring async disk I/O |
| v0.4 | Zero-copy pipeline (mmap + sendfile on TCP, io_uring on QUIC) |
| v0.5 | io_uring registered buffers, buffer pool |
| v0.6-v0.7 | Parallel QUIC streams, Prometheus metrics |
| v0.8 | quiche + io_uring full I/O path control |
| v1.0 | Benchmarks, documentation, public release |
| v1.x | eBPF/XDP fast path, NAT traversal, multi-seeder |

## Current Status

**v0.1 — TCP whole-file transfer**

- [x] Project skeleton, CMake, GoogleTest
- [x] Pluggable `Transport` interface
- [x] `TcpTransport` + `tcp_connect` / `tcp_listen` / `tcp_accept` factories
- [x] `SocketFd` RAII wrapper
- [x] Custom binary wire protocol (`protocol.hpp/.cpp`)
- [x] `Buffer` — move-only, unified heap/mmap/io_uring backing
- [x] Whole-file push/pull over TCP
- [x] CLI: `embr push <file>` / `embr pull <ip>`
- [x] Protocol unit tests

## License

[Mozilla Public License 2.0](https://www.mozilla.org/en-US/MPL/2.0/) — Modify embr's files → your changes must be open source. Use embr in your own project → your new files can be any license.

---

Key updates:
- Status reflects v0.1 complete with all checkboxes ticked
- Usage updated to current CLI (`push <file>` / `pull <ip>`)
- Architecture reflects actual file structure
- Wire protocol documented
- Roadmap phases realigned with our updated phase plan
