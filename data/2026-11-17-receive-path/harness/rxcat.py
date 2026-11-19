#!/usr/bin/env python3
# rxcat.py <label>=<perf.data> ...  receiver CPU seconds per transfer, by what the code is for (task-clock, 50 us/sample, kernel call chains)
import re, subprocess, sys, collections
ORDER = ['user: SHA-256 of the file', 'user: AEAD decrypt (wolfSSL)', 'user: ngtcp2', 'user: embr loop, memcpy, clock',
         'k: recvmsg', 'k: select', 'k: send ACKs: syscall + UDP/IP output', 'k: send ACKs: loopback delivery to the sender',
         'k: SEND_ZC notifications', 'k: file pages (mmap destination)', 'k: file syscalls', 'k: interrupts, timers', 'other']
def classify(stack):
    # stack: list of (sym, dso), leaf first
    syms = [s for s, _ in stack]; leaf_dso = stack[0][1] if stack else ''
    has = lambda *names: any(any(s.startswith(n) for n in names) for s in syms)
    kernel = leaf_dso.startswith('[') and leaf_dso != '[vdso]'
    if not kernel:
        if 'libcrypto' in leaf_dso: return ORDER[0]
        if 'libwolfssl' in leaf_dso: return ORDER[1]
        if 'libngtcp2' in leaf_dso: return ORDER[2]
        return ORDER[3]
    if has('__x64_sys_recvmsg', '__sys_recvmsg'): return ORDER[4]
    if has('__x64_sys_select', '__x64_sys_pselect6', 'core_sys_select', 'do_select', 'kern_select'): return ORDER[5]
    if has('__x64_sys_io_uring_enter', '__do_sys_io_uring_enter', 'io_submit_sqes', 'io_send_zc', 'io_issue_sqe', 'udp_sendmsg', '__x64_sys_sendmsg', '__x64_sys_sendto', '__sys_sendmsg'):
        if has('io_tx_ubuf', 'io_notif', 'tctx_task_work', 'io_handle_tw', 'task_work_run', '__io_run_local_work', 'io_req_task_work'): return ORDER[8]
        if has('__netif_receive_skb', 'process_backlog', 'net_rx_action', '__napi_poll', 'handle_softirqs', 'do_softirq'): return ORDER[7]
        return ORDER[6]
    if has('task_work_run', 'tctx_task_work', 'io_handle_tw'): return ORDER[8]
    if has('asm_exc_page_fault', 'exc_page_fault', 'do_user_addr_fault'): return ORDER[9]
    if has('__x64_sys_munmap', '__x64_sys_mmap', '__x64_sys_msync', '__x64_sys_fallocate', '__x64_sys_ftruncate', '__x64_sys_write', '__x64_sys_pwrite64',
           '__x64_sys_fsync', '__x64_sys_read', '__x64_sys_pread64', '__x64_sys_openat', '__x64_sys_close', '__x64_sys_exit', '__x64_sys_rename', '__x64_sys_unlink', 'do_exit', 'mmput'): return ORDER[10]
    if has('asm_sysvec', 'asm_common_interrupt', 'sysvec_', 'irq_exit', '__irq_exit'): return ORDER[11]
    return ORDER[12]
def load(f):
    out = subprocess.run(['perf', 'script', '-i', f, '-F', 'ip,sym,dso'], capture_output=True, text=True).stdout
    cats = collections.Counter(); other = collections.Counter(); stack = []
    def flush():
        if stack:
            c = classify(stack); cats[c] += 1
            if c == 'other': other[stack[0][0] + ' <- ' + (stack[-1][0] if stack else '')] += 1
    for line in out.splitlines():
        m = re.match(r'\s+[0-9a-f]+\s+(.+?)\s+\((.*)\)\s*$', line)
        if m: stack.append((re.sub(r'\+0x[0-9a-f]+$', '', m.group(1)), m.group(2).split('/')[-1]))
        elif not line.strip(): flush(); stack = []
    flush()
    return cats, other
runs = [a.split('=', 1) for a in sys.argv[1:] if '=' in a]
res = [load(f) for _, f in runs]
print(f'  {"receiver CPU seconds per GiB":46s}' + ''.join(f'{l:>12s}' for l, _ in runs))
print(f'  {"TOTAL":46s}' + ''.join(f'{sum(c.values()) * 50e-6:12.3f}' for c, _ in res))
for nm in ORDER:
    print(f'  {nm:46s}' + ''.join(f'{c[nm] * 50e-6:12.3f}' for c, _ in res))
if '-v' in sys.argv:
    for (l, _), (_, o) in zip(runs, res):
        print('other in', l); [print(f'   {n * 50e-6:7.3f} {k}') for k, n in o.most_common(12)]
