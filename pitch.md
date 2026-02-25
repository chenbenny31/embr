# **embr — A High-Performance P2P File Transfer Tool**

**Siyue Chen**

*embr* — from Old English *ǣmyrġe*, "A small, glowing, or smoldering piece of coal or wood." A shared file is like asn: still glowing, passed from hand to hand, never fully extinguished.

---

## **Hook**

To transfer a 10GB dataset today, you type:

**`scp -i ~/.ssh/id_rsa user@192.168.1.50:/home/user/data/experiment_results_2026_v3.tar.gz ./`**

You need SSH configured, the correct credentials, the exact remote file path — and then you wait. Because disk I/O and network I/O happen **sequentially**: read a chunk, wait, send it, wait, read the next. Multiple redundant memory copies between kernel and userspace on every operation. The disk idles while the network works, and vice versa. Modern 10G NICs are standard, but this sequential pipeline leaves most of that bandwidth unused.

With **embr**:

**`embr push experiment_results.tar.gz    # sender gets a token: Kf3xQ9mZ`**  
**`embr pull Kf3xQ9mZ                     # receiver downloads directly`**

No SSH, no filepath, no configuration.

**Objective:** Pipeline disk and network I/O in parallel, eliminate redundant copies, and saturate modern links at near-zero CPU overhead.

---

## **Problem Statement & Existing Solutions**

The core bottleneck across all existing tools is **sequential I/O** — disk waits for network, network waits for disk — compounded by redundant kernel-userspace memory copies.

* **scp/sftp:** Requires SSH \+ remote filepath. Single TCP stream, sequential I/O, SSH windowing limitations cap practical throughput well below link speed. No parallelism, no resume.  
* **HTTPS download:** Single TCP stream, HTTP framing overhead. Resume requires Range header support (unreliable). No native parallelism.  
* **BitTorrent:** Designed for mass distribution, not point-to-point speed. TCP head-of-line blocking, complex swarm incentive mechanics — unnecessary for direct transfers.  
* **Facebook WDT:** Closest competitor — parallel TCP connections for throughput. Still uses sequential disk I/O and requires direct IP/port with no discovery.

|  | scp/sftp | HTTPS | BitTorrent | WDT | embr |
| ----- | ----- | ----- | ----- | ----- | ----- |
| Transport | TCP (SSH) | TCP \+ TLS | TCP | Multi-TCP | **QUIC** |
| Zero-copy I/O | ✗ | ✗ | ✗ | ✗ | **✓** |
| Parallel chunks | ✗ | ✗ | ✓ | ✓ | **✓** |
| Resume | ✗ | Optional | ✓ | ✓ | **✓** |
| Simple sharing | ✗ | URL | ✗ | ✗ | **✓ (tokens)** |
| Encrypted | ✓ (SSH) | ✓ (TLS) | Optional | ✗ | **✓ (TLS 1.3)** |

---

## **Our Solution**

**1\. QUIC Transport** — Multiplexed streams over a single connection. One dropped packet doesn't stall other chunks (no head-of-line blocking). Built-in TLS 1.3 with AES-NI hardware acceleration. 0-RTT resumption.

**2\. Pipelined Zero-Copy I/O** — The core contribution. Traditional path: disk → page cache (DMA) → userspace buffer (CPU copy) → socket buffer (CPU copy) → NIC (DMA). Two redundant CPU copies, all sequential. embr uses io\_uring to submit disk reads and network sends to the same async queue: disk reads chunk N while network sends chunk N-1. mmap \+ scatter-gather I/O eliminates the CPU copies. **Sequential with redundant copies → pipelined with zero copy.**

**3\. Pluggable Transport Architecture** — Abstract `Transport` interface decouples business logic from protocol. Phase 1: msquic \+ io\_uring (disk I/O only). Phase 2: migrate to quiche (stateless QUIC parser) \+ eBPF/XDP for selective kernel bypass — full I/O control on both disk and network paths.

**4\. Simple CLI with Tracker Discovery**

* **`embr push <file>`** → registers with tracker, generates token  
* **`embr pull <token>`** → resolves peer via tracker, downloads directly  
* **`embr pull <token> <src-ip>`** → direct P2P, bypasses tracker for discovery; integrity verified via sender-provided hash during handshake

---

## **Implementation & Timeline**

| Phase | Deliverable | When |
| ----- | ----- | ----- |
| v0.1-v0.2 | TCP prototype, chunking, tracker, direct P2P mode | Feb |
| v0.3 | QUIC transport (msquic), io\_uring disk I/O | Mar |
| v0.4-v0.5 | Zero-copy pipeline (mmap \+ io\_uring), buffer pool | Mar-Apr |
| v0.6-v0.7 | Parallel streams, Prometheus metrics | Apr |
| v0.8+ | Migrate to quiche \+ eBPF/XDP full-path I/O | Future |

**Testing:** GoogleTest for protocol/chunk/transport. Integration: transfer → SHA256 verify (both tracker and direct P2P modes). Performance: throughput vs scp/WDT at 1G/10G. Fault injection: kill \-9 resume, packet loss, disk full.

| Metric | scp baseline | embr target | Why |
| ----- | ----- | ----- | ----- |
| Throughput (10G) | \~3 Gbps | **9+ Gbps** | Pipelined I/O \+ zero-copy |
| CPU at line rate | \>90% | **\<30%** | Eliminate redundant copies |
| Resume | From scratch | **Last chunk** | Chunk bitmap on disk |

---

## **Future: Scaling Beyond 1:1**

v1.0 is single seeder, single receiver, single tracker. The architecture is designed with extension points:

**1:N (one seeder → many receivers)** — Each receiver opens an independent QUIC connection. Key challenge: disk serving concurrent readers. Solution: coalesce chunk scheduling so all receivers download similar chunks, keeping disk I/O sequential.

**N:1 (many seeders → one receiver)** — Download different chunks from different seeders. Key challenge: dynamic load balancing — fast seeders get more work (work-stealing). Chunk manager's per-chunk state bitmap extends naturally with a seeder dimension.

**N:N (swarm)** — Receivers become seeders once they hold chunks. Requires incentive/trust model. This is where embr converges toward a modern BitTorrent with QUIC \+ zero-copy.

**Multi-tracker** — Single tracker is a SPOF. Future: trackers share token state via gossip or Raft replication, enabling geographic distribution and fault tolerance.

---

## **Long-Term Vision**

* **Replace scp/sftp** as the default file transfer tool on Linux — same simplicity, 10x the throughput  
* **Supersede BitTorrent** for direct sharing — built on voluntary participation rather than enforced fairness (no tit-for-tat, no choking). Peers share because they choose to, not because the protocol punishes them for not sharing  
* **Become foundational infrastructure for file delivery** — a building block that other tools and services can embed for high-performance data movement, the way libcurl is embedded for HTTP

---

