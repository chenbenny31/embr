#!/usr/bin/env python3
# run inside: unshare --user --map-root-user --net python3 drive.py <out.tsv> [quick]
import os, re, statistics, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
B = os.path.join(here, 'egress_bench')
sh = lambda c: subprocess.run(c, shell=True, check=False, capture_output=True, text=True)
sh('ip link set lo up; ip link add dummy0 type dummy; ip addr add 10.99.0.1/24 dev dummy0; ip link set dummy0 up')
CPU = os.environ.get('CPU', '4'); REPS = int(os.environ.get('REPS', '5'))
out = open(sys.argv[1], 'a')
def run(dev, mode, pkt, burst, gso, segs=16, flags='0'):
    dst = {'lo': '127.0.0.1', 'dummy': '10.99.0.2'}[dev]
    per_send = min(burst, segs) if gso else 1
    n = int(1_500_000 * (1 if per_send == 1 else min(per_send, 12)))   # about 0.4-0.8 s each
    rows = []
    for _ in range(REPS):
        r = sh(f'SEGS={segs} taskset -c {CPU} {B} {mode} {dst} 19001 {pkt} {burst} {gso} {n} {flags}')
        m = dict(re.findall(r'(\w+)=([\w.]+)', r.stdout.split('ns_pkt:')[0] + ' ' + r.stdout.split('user=')[-1]))
        if 'wall' not in m: print('FAIL', r.stdout, r.stderr); return
        rows.append((float(m['wall']) * 1e9 / n, float(m['sys']) * 1e9 / n, float(m['user']) * 1e9 / n,
                     float(m['softirq']) * 1e9 / n, int(m['sends']), int(m['copied']), int(m.get('errs', 0))))
    med = lambda i: statistics.median(x[i] for x in rows)
    sends = rows[0][4]
    line = (f'{dev}\t{mode}\t{flags}\t{pkt}\t{burst}\t{gso}\t{segs}\t{n/sends:.1f}\t{med(0):.1f}\t{med(1):.1f}\t{med(2):.1f}\t'
            f'{med(3):.1f}\t{med(0)*n/sends:.0f}\t{rows[0][5]*100//sends}\t{max(x[6] for x in rows)}')
    print(line, flush=True); out.write(line + '\n'); out.flush()
print('dev\tmode\tflags\tpkt\tburst\tgso\tsegs\tpkts/send\twall_ns/pkt\tsys\tuser\tsoftirq\twall_ns/send\tcopied%\terrs')
which = sys.argv[2] if len(sys.argv) > 2 else 'main'
if which == 'main':
    for dev in ('dummy', 'lo'):
        for burst, gso in ((2, 0), (2, 1), (10, 0), (10, 1), (16, 1), (32, 1)):
            for mode in ('sendmsg', 'usend', 'zc'):
                run(dev, mode, 1200, burst, gso)
elif which == 'size':      # where does the saved copy start to pay: bigger sends on the ideal device
    for pkt, segs in ((1200, 4), (1200, 8), (1200, 16), (1350, 16), (1200, 32), (1350, 32), (1200, 48), (1350, 47)):
        for mode in ('sendmsg', 'zc'):
            run('dummy', mode, pkt, segs, 1, segs)
elif which == 'flags':     # ring setup flags: default, COOP_TASKRUN|TASKRUN_FLAG, SINGLE_ISSUER|DEFER_TASKRUN|TASKRUN_FLAG
    for dev in ('dummy', 'lo'):
        for burst, gso in ((2, 0), (2, 1), (10, 1), (16, 1)):
            for flags in ('0', '300', '3200'):
                run(dev, 'zc', 1200, burst, gso, 16, flags)
elif which == 'r816':      # SEND_ZC vs sendmsg at 8-16 packets per send, and on to the crossing point
    for segs in (8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 47):
        for mode in ('sendmsg', 'zc'):
            run('dummy', mode, 1350, segs, 1, segs)
