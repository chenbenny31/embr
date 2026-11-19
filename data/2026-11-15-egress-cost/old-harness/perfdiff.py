#!/usr/bin/env python3
# inside the private netns: profile sendmsg and zc for one cell, print ns per send by kernel/user symbol
import os, re, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__)); B = os.path.join(here, 'egress_bench')
sh = lambda c: subprocess.run(c, shell=True, capture_output=True, text=True)
sh('ip link set lo up; ip link add dummy0 type dummy; ip addr add 10.99.0.1/24 dev dummy0; ip link set dummy0 up')
def profile(dev, mode, pkt, burst, gso, segs, n, flags='0'):
    dst = {'lo': '127.0.0.1', 'dummy': '10.99.0.2'}[dev]
    tag = f'{dev}-{mode}-{pkt}-{burst}-{gso}-{segs}-{flags}'; data = os.path.join(here, 'perf', tag + '.data')
    r = sh(f'SEGS={segs} perf record -q -F 25000 -o {data} -- taskset -c 4 {B} {mode} {dst} 19001 {pkt} {burst} {gso} {n} {flags}')
    m = dict(re.findall(r'(\w+)=([\w.]+)', r.stdout.split('ns_pkt:')[0]))
    if 'wall' not in m: print('FAIL', r.stdout[-300:], r.stderr[-300:]); sys.exit(1)
    per_send = float(m['wall']) * 1e9 / int(m['sends'])
    rep = sh(f'perf report -i {data} --stdio --no-children --sort sym -F overhead,sym 2>/dev/null').stdout
    syms = {}
    for line in rep.splitlines():
        mm = re.match(r'\s*([\d.]+)%\s+\[(.)\]\s+(\S+)', line)
        if mm: syms[(mm.group(2), mm.group(3))] = float(mm.group(1)) / 100 * per_send
    return per_send, syms
dev, pkt, burst, gso, segs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
a_mode, b_mode = (sys.argv[6], sys.argv[7]) if len(sys.argv) > 7 else ('sendmsg', 'zc')
per = min(burst, segs) if gso else 1
n = 3_000_000 * (1 if per == 1 else min(per, 12))
pa, sa = profile(dev, a_mode, pkt, burst, gso, segs, n); pb, sb = profile(dev, b_mode, pkt, burst, gso, segs, n)
print(f'== {dev} pkt={pkt} burst={burst} gso={gso} segs={segs}: ns per send  {a_mode}={pa:.0f}  {b_mode}={pb:.0f}  delta={pb-pa:+.0f}')
keys = sorted(set(sa) | set(sb), key=lambda k: -abs(sb.get(k, 0) - sa.get(k, 0)))
top = int(os.environ.get('TOP', '28')); shown = 0.0
for k in keys[:top]:
    d = sb.get(k, 0) - sa.get(k, 0); shown += d
    print(f'  {d:+7.1f}  {sa.get(k,0):7.1f} -> {sb.get(k,0):7.1f}  [{k[0]}] {k[1]}')
print(f'  (rows shown sum to {shown:+.0f} of {pb-pa:+.0f})')
