#!/usr/bin/env python3
# inside the private netns: profile sendmsg / usend / zc per cell, print ns per send grouped by what the code is for
import os, re, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__)); B = os.path.join(here, 'egress_bench')
sh = lambda c: subprocess.run(c, shell=True, capture_output=True, text=True)
sh('ip link set lo up; ip link add dummy0 type dummy; ip addr add 10.99.0.1/24 dev dummy0; ip link set dummy0 up')
CATS = [  # first match wins
 ('copy payload',        r'^k:(_copy_from_iter|copy_user|rep_movs|__memcpy|memcpy|skb_copy_ubufs|copy_page|csum_partial_copy|csum_and_copy|skb_copy_datagram_from_iter|ip_generic_getfrag|skb_copy_bits|clear_page|rep_stos|__skb_copy|skb_do_copy_data_nocache)'),
 ('zc notification',     r'^k:(io_notif|io_tx_ubuf|tctx_task_work|task_work|io_req_.*work|__io_req_task_work|io_handle_tw|ctx_flush_and_put|io_link_skb|io_ubuf|msg_zerocopy|skb_zcopy|io_sg_from_iter|io_send_zc_cleanup|mm_account_pinned|__zerocopy_sg|skb_zerocopy|io_alloc_notif|exit_to_user_mode_loop|io_fallback_tw|io_run_task_work|io_cqe_cache_refill|io_req_post_cqe|io_post_aux_cqe|__io_post_aux|io_req_task_complete|io_free_req|resume_user_mode_work|__io_run_local)'),
 ('zc buffer import',    r'^k:(io_import_fixed|io_import_reg|io_send_zc_import|io_rsrc|io_find_buf_node|io_put_rsrc|io_req_assign_buf|io_buffer|iov_iter_bvec|io_vec|bvec)'),
 ('io_uring submit',     r'^k:(io_|__io_|__do_sys_io_uring|__x64_sys_io_uring|mutex_|__mutex|percpu_ref|io_uring)'),
 ('sendmsg entry',       r'^k:(__x64_sys_sendmsg|__sys_sendmsg|___sys_sendmsg|____sys_sendmsg|copy_msghdr|copy_iovec|_copy_from_user|__check_object_size|check_heap_object|__import_iovec|import_iovec|sockfd_lookup|fdget|__fget|fput|security_socket_sendmsg|__virt_addr_valid|check_stack_object|iovec_from_user)'),
 ('syscall entry/exit',  r'^k:(entry_SYSCALL|do_syscall|syscall_|entry_SYSRETQ|x64_sys_call|arch_exit_to_user|__x86_indirect|native_|srso_|retbleed|its_return|clear_bhb|amd_clear|fpregs|switch_fpu|restore_fpregs)'),
 ('page/skb alloc+free', r'^k:(kmem_cache|__kmalloc|kmalloc|kfree|skb_release|__alloc_skb|alloc_skb|__build_skb|sock_alloc_send|sock_wfree|skb_free_head|__slab|___slab|alloc_pages|__alloc_pages|get_page_from_freelist|free_unref|__free_pages|free_frozen|__folio_put|page_frag|skb_page_frag|rmqueue|__rmqueue|post_alloc|free_pages|___pte|__put_page|put_page|__page_frag|napi_consume|consume_skb|sk_skb_reason|__kfree_skb|kfree_skb|skb_attempt|__free_frozen|free_tail|__mod_|mod_|lruvec|memcg|obj_cgroup|refill_|try_charge|__memcg|memset|__skb_clone|skb_clone|pskb_expand|__pskb)'),
 ('loopback receive',    r'^k:(__netif_receive|netif_receive|ip_rcv|ip_local_deliver|ip_protocol_deliver|udp_rcv|__udp4_lib|udp_queue_rcv|udp_unicast|udp_rcv_segment|__udp_gso|skb_segment|__skb_gso|skb_gso|inet_gso|udp4_ufo|process_backlog|net_rx_action|handle_softirqs|__do_softirq|do_softirq|__napi_poll|enqueue_to_backlog|__netif_rx|netif_rx|loopback_xmit|ip_route_input|ip_rcv_finish|udp4_lib_lookup|__udp_enqueue|skb_consume_udp|raw_local_deliver|ip_sublist|ip_list|sk_filter|__skb_checksum|skb_checksum|ipv4_pktinfo|skb_orphan_frags_rx|eth_type_trans|udp_gro|dev_gro|napi_gro|inet_gro|skb_gro|sk_.*rcv|sock_def|__udp_queue|udp_csum|skb_mac_gso|skb_udp_tunnel|__sk_mem|sk_rmem|sock_queue|udp_lib_checksum|udp_v4_early|udp4_csum|inet_proto_csum)'),
 ('user: egress code',   r'^\.:(ZcEgress|__io_uring|io_uring|_io_uring|std::|Buffer|__libc_sendmsg|sendmsg|__syscall|syscall|operator|_ZN)'),
 ('user: fill packet',   r'^\.:(main|__memmove|__memcpy|memcpy|__mempcpy)'),
]
def cat(k):
    for name, rx in CATS:
        if re.match(rx, k): return name
    return 'UDP/IP send path' if k.startswith('k:') else 'user: other'
def profile(dev, mode, pkt, burst, gso, segs, n, flags='0'):
    dst = {'lo': '127.0.0.1', 'dummy': '10.99.0.2'}[dev]
    tag = f'{dev}-{mode}-{pkt}-{burst}-{gso}-{segs}-{flags}'; data = os.path.join(here, 'perf', tag + '.data')
    r = sh(f'SEGS={segs} perf record -q -F 25000 -o {data} -- taskset -c 4 {B} {mode} {dst} 19001 {pkt} {burst} {gso} {n} {flags}')
    m = dict(re.findall(r'(\w+)=([\w.]+)', r.stdout.split('ns_pkt:')[0]))
    if 'wall' not in m: print('FAIL', r.stdout[-300:], r.stderr[-300:]); sys.exit(1)
    per_send = float(m['wall']) * 1e9 / int(m['sends'])
    rep = sh(f'perf report -i {data} --stdio --no-children --sort sym -F overhead,sym 2>/dev/null').stdout
    cats, syms = {}, {}
    for line in rep.splitlines():
        mm = re.match(r'\s*([\d.]+)%\s+\[(.)\]\s+(\S+)', line)
        if not mm: continue
        k = mm.group(2) + ':' + mm.group(3); v = float(mm.group(1)) / 100 * per_send
        cats[cat(k)] = cats.get(cat(k), 0) + v; syms[k] = v
    return per_send, cats, syms
dev, pkt, burst, gso, segs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
modes = sys.argv[6].split(',') if len(sys.argv) > 6 else ['sendmsg', 'usend', 'zc']
per = min(burst, segs) if gso else 1
n = 3_000_000 * (1 if per == 1 else min(per, 12))
res = [profile(dev, m, pkt, burst, gso, segs, n) for m in modes]
print(f'== {dev}: {per} x {pkt} B per send ({per*pkt/1000:.1f} kB), {burst} packets per flush.  ns per send:')
print(f'  {"":22s}' + ''.join(f'{m:>9s}' for m in modes))
print(f'  {"TOTAL":22s}' + ''.join(f'{r[0]:9.0f}' for r in res))
names = [c[0] for c in CATS] + ['UDP/IP send path', 'user: other']
for nm in names:
    vals = [r[1].get(nm, 0) for r in res]
    if max(vals) >= 0.5: print(f'  {nm:22s}' + ''.join(f'{v:9.1f}' for v in vals))
if os.environ.get('UNCAT'):
    for i, m in enumerate(modes):
        other = sorted(((v, k) for k, v in res[i][2].items() if cat(k) in ('UDP/IP send path', 'user: other')), reverse=True)[:int(os.environ['UNCAT'])]
        print(f'  [{m}] largest uncategorised:', ', '.join(f'{k[2:]} {v:.0f}' for v, k in other))
