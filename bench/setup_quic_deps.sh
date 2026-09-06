#!/usr/bin/env bash
# setup_quic_deps.sh — build wolfSSL + ngtcp2 the way embr's QUIC path needs them
# Run from anywhere: bash bench/setup_quic_deps.sh            (deps + sysctl)
#                    BUILD_EMBR=1 bash bench/setup_quic_deps.sh (then release-build embr in ./build)
#
# Reproduces the exact configure lines recorded in the reference build's config.log.
# The flags that are NOT optional, and why:
#   --enable-quic           ngtcp2 crypto backend hooks
#   --enable-opensslextra   ngtcp2_crypto_wolfssl needs wolfSSL_EVP_aes_{128,256}_ecb
#   --enable-aesecb         same (header protection)
#   --enable-aesgcm-stream  without it wolfSSL's EVP layer memcpys the whole plaintext
#                           into ctx->authBuffer and encrypts one-shot from the copy:
#                           a hidden ~1 GiB memcpy per GiB sent, invisible to strace.
#                           Every copy-floor claim in bench.md assumes this flag.
#   --enable-aesni --enable-intelasm   x86_64 only; AES-GCM at line rate
# Install prefixes match CMakeLists.txt: ~/.local/wolfssl and ~/.local/ngtcp2
#
# The SEND_ZC egress (io_uring, liburing-devel above) additionally needs a kernel >= 6.2
# (IORING_SEND_ZC_REPORT_USAGE) and ~105 KiB of RLIMIT_MEMLOCK per QUIC connection; the
# stock 8 MiB is plenty. Neither is fatal here: embr falls back to sendmsg, but the
# embrquic bench row would then measure the fallback. verify() prints both.
#
# Also raises net.core.{r,w}mem_max: embr asks for 4 MiB UDP socket buffers and the
# kernel silently clamps to these (212992 on Amazon Linux 2023). Below ~1 MiB the
# receiver's per-chunk SHA-256 stall overflows the buffer at WAN rates and every chunk
# becomes a loss event. This is the single most likely cause of an "ugly" QUIC number.

set -euo pipefail

WOLFSSL_REF="${WOLFSSL_REF:-v5.9.2-stable}"   # reference laptop build: v5.9.2-stable-1862-gfeca5c424 (master)
NGTCP2_REF="${NGTCP2_REF:-v1.22.0}"
SRC="${SRC:-$HOME/src}"
PREFIX_WOLFSSL="${PREFIX_WOLFSSL:-$HOME/.local/wolfssl}"
PREFIX_NGTCP2="${PREFIX_NGTCP2:-$HOME/.local/ngtcp2}"
JOBS="${JOBS:-$(nproc)}"
FORCE="${FORCE:-0}"
BUILD_EMBR="${BUILD_EMBR:-0}"
EMBR_BUILD_DIR="${EMBR_BUILD_DIR:-./build}"
SOCKBUF_MAX="${SOCKBUF_MAX:-16777216}"        # 16 MiB; embr requests 4 MiB
SYSCTL_FILE="${SYSCTL_FILE:-/etc/sysctl.d/90-embr-quic.conf}"

log() { echo "[setup_quic_deps $(date +%H:%M:%S)] $*" >&2; }
die() { log "FATAL: $*"; exit 1; }

install_system_deps() {
    if command -v dnf >/dev/null; then          # Fedora / Amazon Linux 2023
        sudo dnf install -y gcc-c++ make cmake ninja-build autoconf automake libtool \
            pkgconf-pkg-config git openssl openssl-devel liburing-devel time nmap-ncat
    elif command -v apt-get >/dev/null; then    # Debian / Ubuntu
        sudo apt-get update
        sudo apt-get install -y build-essential cmake ninja-build autoconf automake libtool \
            pkg-config git openssl libssl-dev liburing-dev time ncat
    else
        die "unknown package manager — install: g++ cmake ninja autoconf automake libtool pkg-config git openssl-devel liburing-devel time ncat"
    fi
}

# embr is C++20 and uses std::jthread/stop_token: libstdc++ >= 10. AL2023 ships GCC 11.
require_gcc() {
    command -v g++ >/dev/null || die "g++ missing"
    local major; major=$(g++ -dumpversion | cut -d. -f1)
    (( major >= 10 )) || die "g++ $major too old for C++20 (need >= 10; AL2023 has 11, or install gcc14-c++)"
    log "g++ $(g++ -dumpversion) ok"
}

# SEND_ZC prerequisites; warn only, the deps and the sendmsg path still work without them
check_sendzc() {
    local k l; k=$(uname -r); l=$(ulimit -l)
    local maj="${k%%.*}" min; min="${k#*.}"; min="${min%%.*}"
    if (( maj > 6 || (maj == 6 && min >= 2) )); then log "kernel $k ok for SEND_ZC (>= 6.2)"
    else log "WARN: kernel $k < 6.2 — SEND_ZC REPORT_USAGE unsupported; embr will copy-fallback, bench rows will refuse"; fi
    if [[ "$l" == "unlimited" ]] || (( l >= 1024 )); then log "memlock ${l} KiB ok for SEND_ZC (>= 1 MiB)"
    else log "WARN: ulimit -l = $l KiB < 1 MiB — SEND_ZC slab cannot register; raise in /etc/security/limits.d or bench rows will refuse"; fi
}

tune_sysctl() {
    local cur_r cur_w
    cur_r=$(sysctl -n net.core.rmem_max); cur_w=$(sysctl -n net.core.wmem_max)
    if (( cur_r >= SOCKBUF_MAX && cur_w >= SOCKBUF_MAX )); then
        log "sysctl already ok: rmem_max=$cur_r wmem_max=$cur_w"; return 0
    fi
    log "raising net.core.rmem_max/wmem_max $cur_r/$cur_w -> $SOCKBUF_MAX (persisted in $SYSCTL_FILE)"
    printf 'net.core.rmem_max = %s\nnet.core.wmem_max = %s\n' "$SOCKBUF_MAX" "$SOCKBUF_MAX" \
        | sudo tee "$SYSCTL_FILE" >/dev/null
    sudo sysctl -q -w net.core.rmem_max="$SOCKBUF_MAX" net.core.wmem_max="$SOCKBUF_MAX"
}

wolfssl_ok() {
    local opts="$PREFIX_WOLFSSL/include/wolfssl/options.h"
    [[ -f "$opts" ]] || return 1
    grep -q WOLFSSL_AESGCM_STREAM "$opts" || return 1
    grep -q WOLFSSL_QUIC "$opts"          || return 1
    grep -q OPENSSL_EXTRA "$opts"         || return 1
    ls "$PREFIX_WOLFSSL"/lib/libwolfssl.so* >/dev/null 2>&1
}

ngtcp2_ok() {
    [[ -f "$PREFIX_NGTCP2/lib/pkgconfig/libngtcp2_crypto_wolfssl.pc" ]] \
        && ls "$PREFIX_NGTCP2"/lib/libngtcp2.so* >/dev/null 2>&1
}

clone_at() { # clone_at <url> <ref> <dir>
    local url="$1" ref="$2" dir="$3"
    if [[ -d "$dir/.git" ]]; then
        git -C "$dir" fetch --tags --quiet
    else
        git clone --quiet "$url" "$dir"
    fi
    git -C "$dir" checkout --quiet "$ref"
    log "$(basename "$dir") @ $(git -C "$dir" describe --tags --always)"
}

build_wolfssl() {
    if [[ "$FORCE" != "1" ]] && wolfssl_ok; then
        log "wolfSSL already correct at $PREFIX_WOLFSSL — skip (FORCE=1 to rebuild)"; return 0
    fi
    clone_at https://github.com/wolfSSL/wolfssl.git "$WOLFSSL_REF" "$SRC/wolfssl"
    pushd "$SRC/wolfssl" >/dev/null
    local asm_flags=()
    [[ "$(uname -m)" == "x86_64" ]] && asm_flags=(--enable-aesni --enable-intelasm)
    ./autogen.sh
    ./configure --prefix="$PREFIX_WOLFSSL" \
        --enable-quic --enable-session-ticket --enable-earlydata --enable-harden \
        --enable-opensslextra --enable-aesecb --enable-aesgcm-stream "${asm_flags[@]}"
    make -j"$JOBS"
    make install
    popd >/dev/null
    wolfssl_ok || die "wolfSSL built but options.h lacks a required flag"
}

build_ngtcp2() {
    if [[ "$FORCE" != "1" ]] && ngtcp2_ok; then
        log "ngtcp2 already present at $PREFIX_NGTCP2 — skip (FORCE=1 to rebuild)"; return 0
    fi
    clone_at https://github.com/ngtcp2/ngtcp2.git "$NGTCP2_REF" "$SRC/ngtcp2"
    pushd "$SRC/ngtcp2" >/dev/null
    autoreconf -fi
    # lib-only: skips the example clients, which would otherwise probe for libev/nghttp3
    ./configure PKG_CONFIG_PATH="$PREFIX_WOLFSSL/lib/pkgconfig" \
        --prefix="$PREFIX_NGTCP2" --with-wolfssl --enable-lib-only
    make -j"$JOBS"
    make install
    popd >/dev/null
    ngtcp2_ok || die "ngtcp2 built but libngtcp2_crypto_wolfssl.pc missing — was wolfSSL found?"
}

verify() {
    log "=== verification ==="
    local opts="$PREFIX_WOLFSSL/include/wolfssl/options.h"
    printf '  WOLFSSL_AESGCM_STREAM in options.h: %s (want >=1)\n' "$(grep -c WOLFSSL_AESGCM_STREAM "$opts")"
    printf '  wc_AesGcmEncryptUpdate exported:    %s (want >=1)\n' \
        "$(nm -D "$PREFIX_WOLFSSL"/lib/libwolfssl.so | grep -c wc_AesGcmEncryptUpdate)"
    export PKG_CONFIG_PATH="$PREFIX_WOLFSSL/lib/pkgconfig:$PREFIX_NGTCP2/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    printf '  wolfssl:                  %s\n' "$(pkg-config --modversion wolfssl)"
    printf '  libngtcp2:                %s\n' "$(pkg-config --modversion libngtcp2)"
    printf '  libngtcp2_crypto_wolfssl: %s\n' "$(pkg-config --modversion libngtcp2_crypto_wolfssl)"
    printf '  net.core.rmem_max/wmem_max: %s / %s (want >= 4194304)\n' \
        "$(sysctl -n net.core.rmem_max)" "$(sysctl -n net.core.wmem_max)"
    printf '  liburing:                 %s (SEND_ZC egress; need >= 2.3 for send_zc_fixed)\n' "$(pkg-config --modversion liburing 2>/dev/null || echo missing)"
    printf '  kernel:                   %s (want >= 6.2 for SEND_ZC REPORT_USAGE)\n' "$(uname -r)"
    printf '  memlock (ulimit -l):      %s KiB (SEND_ZC slab ~105 KiB per QUIC connection; want >= 1024)\n' "$(ulimit -l)"
}

build_embr() {
    [[ -f CMakeLists.txt ]] || die "BUILD_EMBR=1 needs to run from the embr repo root"
    cmake -B "$EMBR_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$EMBR_BUILD_DIR" -j"$JOBS" --target embr
    log "embr built: $EMBR_BUILD_DIR/embr"
}

main() {
    mkdir -p "$SRC"
    install_system_deps
    require_gcc
    check_sendzc
    tune_sysctl
    build_wolfssl
    build_ngtcp2
    verify
    [[ "$BUILD_EMBR" == "1" ]] && build_embr
    log "done. Sender host security group: inbound UDP 10008/10009 (QUIC zc/sendmsg) + TCP 10007/9999/22."
}

main "$@"
