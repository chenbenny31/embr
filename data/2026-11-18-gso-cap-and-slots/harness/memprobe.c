// memprobe <mlock_kib> <register_kib> <hold_ms> <tag>: lock memory with mlock() and/or register an io_uring buffer, report, hold
#define _GNU_SOURCE
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: memprobe mlock_kib register_kib hold_ms tag\n"); return 2; }
    size_t ml = (size_t)atol(argv[1]) * 1024, reg = (size_t)atol(argv[2]) * 1024; int hold = atoi(argv[3]); const char* tag = argv[4];
    if (ml) {
        void* p = mmap(NULL, ml, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int r = mlock(p, ml);
        printf("%s: mlock %zu KiB: %s\n", tag, ml / 1024, r == 0 ? "ok" : strerror(errno));
    }
    if (reg) {
        struct io_uring ring; int q = io_uring_queue_init(8, &ring, 0);
        if (q < 0) { printf("%s: ring: %s\n", tag, strerror(-q)); return 1; }
        void* b = mmap(NULL, reg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        memset(b, 1, reg);
        struct iovec iov = { b, reg };
        int r = io_uring_register_buffers(&ring, &iov, 1);
        printf("%s: register %zu KiB with io_uring: %s\n", tag, reg / 1024, r == 0 ? "ok" : strerror(-r));
    }
    fflush(stdout);
    usleep((useconds_t)hold * 1000);
    return 0;
}
