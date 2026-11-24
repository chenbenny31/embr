#!/usr/bin/env bash
# install.sh — system packages, wolfSSL + ngtcp2 built the way embr's QUIC path needs them,
#              UDP socket limits, and a self-signed QUIC cert in ~/.config/embr
# Run from the repo root: bash install.sh               (deps + sysctl + cert)
#                         BUILD_EMBR=1 bash install.sh  (then release-build embr in ./build)
#                         bash install.sh sysctl        (only the sysctl step: lifts the UDP socket-buffer limit)
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
# Also builds Intel's isa-l_crypto into ~/.local/isa-l_crypto (x86_64 only, optional for embr):
# it hashes 8 or 16 chunks side by side with AVX2 or AVX-512, several times faster than one by
# one on a CPU without SHA extensions. Without it embr builds and runs the same, chunk by chunk.
# Its assembly needs nasm; when the system has none, nasm is built into ~/.local/nasm first,
# from its release tarball (checksum pinned below).
#
# Root: everything above goes under $HOME. sudo is used for two things only, and only when they
# are needed: system packages that are missing, and the socket-buffer sysctl when it is too low.
# The sysctl is not required: when sudo is refused or missing the run goes on, and ends with the
# command that lifts the limit. NO_SUDO=1 never calls sudo (missing packages are then an error
# that lists them).
#
# The SEND_ZC egress (io_uring, liburing-devel above) additionally needs a kernel >= 6.2
# (IORING_SEND_ZC_REPORT_USAGE) and ~105 KiB of RLIMIT_MEMLOCK per QUIC connection; the
# stock 8 MiB is plenty. Neither is fatal here: embr falls back to sendmsg, but the
# embrquic bench row would then measure the fallback. verify() prints both.
#
# net.core.{r,w}mem_max: embr asks for 4 MiB UDP socket buffers and the kernel silently clamps
# to these (212992 by default on Fedora, Ubuntu and Amazon Linux 2023). embr runs either way; with
# the default the buffer holds about a millisecond of a fast transfer, and QUIC loses packets there
# whenever the receiver falls behind. This is the single most likely cause of an "ugly" QUIC number.

set -euo pipefail

WOLFSSL_REF="${WOLFSSL_REF:-v5.9.2-stable}"   # reference laptop build: v5.9.2-stable-1862-gfeca5c424 (master)
# >= 1.26.0: earlier releases can leave a server connection silent until the idle timeout
# when both retransmission probes are lost while the congestion window is at its minimum
NGTCP2_REF="${NGTCP2_REF:-v1.26.0}"
ISAL_CRYPTO_REF="${ISAL_CRYPTO_REF:-v2.26.1}"
# built only when the system has no nasm >= 2.14.01; the release tarball needs a C compiler and
# make, nothing else (its git tree wants autoconf >= 2.71, which Amazon Linux 2023 does not have)
NASM_VERSION="${NASM_VERSION:-2.16.03}"
NASM_SHA256="${NASM_SHA256:-5bc940dd8a4245686976a8f7e96ba9340a0915f2d5b88356874890e207bdb581}"
SRC="${SRC:-$HOME/src}"
PREFIX_WOLFSSL="${PREFIX_WOLFSSL:-$HOME/.local/wolfssl}"
PREFIX_NGTCP2="${PREFIX_NGTCP2:-$HOME/.local/ngtcp2}"
PREFIX_ISAL_CRYPTO="${PREFIX_ISAL_CRYPTO:-$HOME/.local/isa-l_crypto}"
PREFIX_NASM="${PREFIX_NASM:-$HOME/.local/nasm}"
NO_SUDO="${NO_SUDO:-0}"
JOBS="${JOBS:-$(nproc)}"
FORCE="${FORCE:-0}"
BUILD_EMBR="${BUILD_EMBR:-0}"
EMBR_BUILD_DIR="${EMBR_BUILD_DIR:-./build}"
SOCKBUF_MAX="${SOCKBUF_MAX:-16777216}"        # what `install.sh sysctl` sets: 16 MiB
SOCKBUF_NEED=4194304                          # what embr asks for (QUIC_SOCKET_BUFFER)
SYSCTL_FILE="${SYSCTL_FILE:-/etc/sysctl.d/90-embr-quic.conf}"

log() { echo "[install $(date +%H:%M:%S)] $*" >&2; }
die() { log "FATAL: $*"; exit 1; }

# what the build cannot do without, as "command" or "pkg:module"; all of it is in the package lists below
REQUIRED_TOOLS=(g++ make cmake ninja autoconf automake libtoolize pkg-config git openssl pkg:openssl pkg:liburing)
BENCH_TOOLS=(/usr/bin/time ncat ethtool)   # bench/*.sh only

missing_of() { # prints the entries of the list that this system lacks
    local tool
    for tool in "$@"; do
        case "$tool" in
            pkg:*) pkg-config --exists "${tool#pkg:}" 2>/dev/null || printf '%s ' "${tool#pkg:}-devel" ;;
            /*)    [[ -x "$tool" ]] || printf '%s ' "$tool" ;;
            *)     command -v "$tool" >/dev/null 2>&1 || printf '%s ' "$tool" ;;
        esac
    done
}

# root only when a required package is missing; the bench tools alone are not worth a password
install_system_deps() {
    local missing bench
    missing=$(missing_of "${REQUIRED_TOOLS[@]}"); bench=$(missing_of "${BENCH_TOOLS[@]}")
    if [[ -z "$missing" ]]; then
        log "system packages present — no root needed for them"
        [[ -z "$bench" ]] || log "WARN: not installed, bench/*.sh needs them: $bench"
        return 0
    fi
    if [[ "$NO_SUDO" == "1" ]] || ! command -v sudo >/dev/null; then
        die "missing: $missing— NO_SUDO=1 (or no sudo here): ask an administrator for the packages listed in install_system_deps"
    fi
    log "missing: $missing— installing system packages with sudo"
    if command -v dnf >/dev/null; then          # Fedora / Amazon Linux 2023
        sudo dnf install -y gcc-c++ make cmake ninja-build autoconf automake libtool \
            pkgconf-pkg-config git openssl openssl-devel liburing-devel time nmap-ncat ethtool nasm
    elif command -v apt-get >/dev/null; then    # Debian / Ubuntu
        sudo apt-get update
        sudo apt-get install -y build-essential cmake ninja-build autoconf automake libtool \
            pkg-config git openssl libssl-dev liburing-dev time ncat ethtool nasm
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

sockbuf_ok() {
    (( $(sysctl -n net.core.rmem_max) >= SOCKBUF_NEED && $(sysctl -n net.core.wmem_max) >= SOCKBUF_NEED ))
}

# raises the limit with sudo; returns 1, and changes nothing, when sudo is switched off, missing or refused
tune_sysctl() {
    local cur_r cur_w rc=0
    cur_r=$(sysctl -n net.core.rmem_max); cur_w=$(sysctl -n net.core.wmem_max)
    if sockbuf_ok; then log "sysctl already ok: rmem_max=$cur_r wmem_max=$cur_w"; return 0; fi
    if [[ "$NO_SUDO" == "1" ]] || ! command -v sudo >/dev/null; then return 1; fi
    log "raising net.core.rmem_max/wmem_max $cur_r/$cur_w -> $SOCKBUF_MAX with sudo (persisted in $SYSCTL_FILE)"
    log "  QUIC loses packets on fast or long paths without it. Ctrl-C at the password prompt skips this step only."
    trap ':' INT # Ctrl-C then ends sudo, not the install
    printf 'net.core.rmem_max = %s\nnet.core.wmem_max = %s\n' "$SOCKBUF_MAX" "$SOCKBUF_MAX" \
        | sudo tee "$SYSCTL_FILE" >/dev/null \
        && sudo sysctl -q -w net.core.rmem_max="$SOCKBUF_MAX" net.core.wmem_max="$SOCKBUF_MAX" || rc=1
    trap - INT
    (( rc == 0 )) || log "WARN: sudo did not go through; the limit stays at $cur_r/$cur_w and the install goes on"
    return $rc
}

# last lines of a run, so they are not lost above the build output
report_sysctl() {
    sockbuf_ok && return 0
    log "one step is left, and it needs root:"
    log "  UDP socket buffers are limited to $(sysctl -n net.core.rmem_max) bytes here; embr asks for $SOCKBUF_NEED."
    log "  embr runs as it is. QUIC transfers on fast or long paths lose packets and slow down until this is run, once:"
    log "      bash install.sh sysctl          (uses sudo)"
    log "  or, as root:  sysctl -w net.core.rmem_max=$SOCKBUF_MAX net.core.wmem_max=$SOCKBUF_MAX"
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
        && ls "$PREFIX_NGTCP2"/lib/libngtcp2.so* >/dev/null 2>&1 || return 1
    # a pinned release that differs from the installed one means rebuild
    [[ "$NGTCP2_REF" != v[0-9]* ]] && return 0
    [[ "v$(PKG_CONFIG_PATH="$PREFIX_NGTCP2/lib/pkgconfig" pkg-config --modversion libngtcp2 2>/dev/null)" == "$NGTCP2_REF" ]]
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
    git clean -fdxq   # objects from another release must not survive a version change
    autoreconf -fi
    # lib-only: skips the example clients, which would otherwise probe for libev/nghttp3
    ./configure PKG_CONFIG_PATH="$PREFIX_WOLFSSL/lib/pkgconfig" \
        --prefix="$PREFIX_NGTCP2" --with-wolfssl --enable-lib-only
    make -j"$JOBS"
    make install
    popd >/dev/null
    ngtcp2_ok || die "ngtcp2 built but libngtcp2_crypto_wolfssl.pc missing — was wolfSSL found?"
}

isal_crypto_ok() {
    [[ -f "$PREFIX_ISAL_CRYPTO/lib/pkgconfig/libisal_crypto.pc" ]] || return 1
    [[ "$ISAL_CRYPTO_REF" != v[0-9]* ]] && return 0
    [[ "v$(PKG_CONFIG_PATH="$PREFIX_ISAL_CRYPTO/lib/pkgconfig" pkg-config --modversion libisal_crypto 2>/dev/null)" == "$ISAL_CRYPTO_REF" ]]
}

nasm_ok() { # <nasm binary>: exists and is >= 2.14.01
    local version
    [[ -n "$1" && -x "$1" ]] || return 1
    version=$("$1" -v 2>/dev/null | awk '{print $3}')
    [[ -n "$version" && "$(printf '%s\n' 2.14.01 "$version" | sort -V | head -1)" == "2.14.01" ]]
}

# prints the nasm to build isa-l_crypto with: the system's, or one built from source under $HOME
ensure_nasm() {
    local system_nasm; system_nasm=$(command -v nasm 2>/dev/null || true)
    if nasm_ok "$system_nasm"; then echo "$system_nasm"; return 0; fi
    if [[ "$FORCE" != "1" ]] && nasm_ok "$PREFIX_NASM/bin/nasm"; then echo "$PREFIX_NASM/bin/nasm"; return 0; fi
    local name="nasm-$NASM_VERSION" url tarball
    url="https://www.nasm.us/pub/nasm/releasebuilds/$NASM_VERSION/$name.tar.gz"; tarball="$SRC/$name.tar.gz"
    log "no nasm >= 2.14.01 on this system — building $name into $PREFIX_NASM"
    if command -v curl >/dev/null; then curl -fsSL -o "$tarball" "$url" || die "cannot download $url"
    elif command -v wget >/dev/null; then wget -q -O "$tarball" "$url" || die "cannot download $url"
    else die "nasm is missing and neither curl nor wget is here to fetch it"; fi
    echo "$NASM_SHA256  $tarball" | sha256sum -c --quiet - >&2 || die "$tarball does not match its pinned sha256"
    rm -rf "${SRC:?}/$name"
    tar -xzf "$tarball" -C "$SRC" || die "cannot unpack $tarball"
    ( cd "$SRC/$name" && ./configure --prefix="$PREFIX_NASM" && make -j"$JOBS" nasm ) >&2 || die "nasm did not build"
    install -D -m 755 "$SRC/$name/nasm" "$PREFIX_NASM/bin/nasm" || die "cannot install nasm" # the binary alone
    nasm_ok "$PREFIX_NASM/bin/nasm" || die "nasm built but $PREFIX_NASM/bin/nasm does not run"
    echo "$PREFIX_NASM/bin/nasm"
}

# optional for embr; its assembly needs nasm >= 2.14.01
build_isal_crypto() {
    if [[ "$(uname -m)" != "x86_64" ]]; then
        log "isa-l_crypto: skipped on $(uname -m); embr hashes chunk by chunk"; return 0
    fi
    if [[ "$FORCE" != "1" ]] && isal_crypto_ok; then
        log "isa-l_crypto already present at $PREFIX_ISAL_CRYPTO — skip (FORCE=1 to rebuild)"; return 0
    fi
    local nasm; nasm=$(ensure_nasm)
    log "nasm: $nasm ($("$nasm" -v))"
    clone_at https://github.com/intel/isa-l_crypto.git "$ISAL_CRYPTO_REF" "$SRC/isa-l_crypto"
    pushd "$SRC/isa-l_crypto" >/dev/null
    git clean -fdxq
    ./autogen.sh
    PATH="$(dirname "$nasm"):$PATH" ./configure --prefix="$PREFIX_ISAL_CRYPTO"
    PATH="$(dirname "$nasm"):$PATH" make -j"$JOBS"
    make install
    popd >/dev/null
    isal_crypto_ok || die "isa-l_crypto built but libisal_crypto.pc is missing or has another version"
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
    printf '  libisal_crypto:           %s\n' \
        "$(PKG_CONFIG_PATH="$PREFIX_ISAL_CRYPTO/lib/pkgconfig" pkg-config --modversion libisal_crypto 2>/dev/null || echo 'not installed: chunks are hashed one by one')"
    printf '  net.core.rmem_max/wmem_max: %s / %s (want >= 4194304)\n' \
        "$(sysctl -n net.core.rmem_max)" "$(sysctl -n net.core.wmem_max)"
    printf '  liburing:                 %s (SEND_ZC egress; need >= 2.4 for SEND_ZC usage reporting)\n' "$(pkg-config --modversion liburing 2>/dev/null || echo missing)"
    printf '  kernel:                   %s (want >= 6.2 for SEND_ZC REPORT_USAGE)\n' "$(uname -r)"
    printf '  memlock (ulimit -l):      %s KiB (SEND_ZC slab ~105 KiB per QUIC connection; want >= 1024)\n' "$(ulimit -l)"
}

# embr push --transport quic reads its default pair from here (same dir as the trusted tracker)
ensure_cert() {
    local dir="$HOME/.config/embr"
    local cert="$dir/cert.pem" key="$dir/key.pem"
    if [[ -f "$cert" && -f "$key" ]]; then log "QUIC cert already present: $cert"; return 0; fi
    mkdir -p "$dir"
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=embr \
        -keyout "$key" -out "$cert" 2>/dev/null || die "openssl req failed"
    chmod 600 "$key"
    log "created self-signed QUIC cert: $cert"
}

build_embr() {
    [[ -f CMakeLists.txt ]] || die "BUILD_EMBR=1 needs to run from the embr repo root"
    cmake -B "$EMBR_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$EMBR_BUILD_DIR" -j"$JOBS" --target embr
    log "embr built: $EMBR_BUILD_DIR/embr"
}

main() {
    if [[ "${1:-}" == "sysctl" ]]; then tune_sysctl || { report_sysctl; return 1; }; return 0; fi
    mkdir -p "$SRC"
    install_system_deps
    require_gcc
    check_sendzc
    tune_sysctl || true # asked for while the user is still at the keyboard; a refusal is reported at the end
    build_wolfssl
    build_ngtcp2
    build_isal_crypto
    verify
    ensure_cert
    [[ "$BUILD_EMBR" == "1" ]] && build_embr
    log "done. Sender host security group: inbound UDP 10008-10012 (one per QUIC bench row) + TCP 10007/9999/22."
    report_sysctl
}

main "$@"
