# embr

A high-performance peer-to-peer file transfer tool built with C++20, QUIC, and zero-copy I/O.

*From Old English ǣmyrġe, "smoldering ash." A shared file is like an ember: still glowing, passed from hand to hand, never fully extinguished.*

## Why

Existing tools arrange disk I/O and network I/O sequentially — the disk waits for the network, the network waits for the disk. Add redundant memory copies between kernel and userspace, and even 10G links stay half-idle.

embr pipelines disk and network operations in parallel and eliminates intermediate copies, targeting near-line-rate throughput at minimal CPU overhead.

## Usage

```bash
# Sender: share a file, get a token
embr push large_dataset.tar.gz
# → Token: Kf3xQ9mZ

# Receiver: download via tracker
embr pull Kf3xQ9mZ

# Receiver: direct P2P (no tracker)
embr pull Kf3xQ9mZ 192.168.1.50
```

## How It Works

Files are split into 16MB chunks transferred in parallel over multiplexed QUIC streams. A lightweight tracker server handles peer discovery via capability tokens. Integrity is verified per-chunk using SHA256.

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

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

### Dependencies

- C++20 compiler (GCC 12+ / Clang 15+)
- CMake 3.20+
- OpenSSL
- GoogleTest (fetched via CMake FetchContent)

## Architecture

```
embr/
├── src/
│   ├── main.cpp
│   ├── core/           # chunk_manager, buffer_pool, hash
│   ├── transport/
│   │   ├── transport.hpp        # abstract interface
│   │   ├── tcp_transport.cpp    # v0.1-v0.2
│   │   ├── msquic_transport.cpp # v0.3
│   │   └── quiche_transport.cpp # v0.8+
│   ├── io/             # io_uring_ctx, mmap_file
│   ├── tracker/        # tracker_client, token
│   └── util/           # metrics, logger
├── tracker/            # standalone tracker server
├── tests/
└── benchmarks/
```

Business logic talks to the `Transport` interface, never to raw sockets. Swapping TCP → QUIC → quiche is a one-file change.

## Roadmap

| Phase | What |
|-------|------|
| v0.1-v0.2 | TCP prototype, chunking, tracker, direct P2P |
| v0.3 | QUIC transport (msquic), io_uring async disk I/O |
| v0.4-v0.5 | Zero-copy pipeline (mmap + io_uring), buffer pool |
| v0.6-v0.7 | Parallel streams, Prometheus metrics |
| v0.8 | Migrate to quiche, io_uring for disk + network |
| v1.0 | Benchmarks, documentation, public release |
| v1.x | eBPF/XDP fast path, NAT traversal, multi-seeder |

## Current Status

**v0.1 — TCP prototype in progress.**

- [x] Project skeleton, CMake, GoogleTest
- [ ] TCP server/client
- [ ] Wire protocol (`[version:u8][type:u8][len:u32][payload]`)
- [ ] Single file transfer
- [ ] Chunked transfer with SHA256 verification
- [ ] Tracker (token registration + resolution)
- [ ] CLI: `embr push` / `embr pull`

## License

[Mozilla Public License 2.0](https://www.mozilla.org/en-US/MPL/2.0/)

Modify embr's files → your changes must be open source. Use embr in your own project → your new files can be any license.
