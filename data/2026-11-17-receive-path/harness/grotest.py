#!/usr/bin/env python3
# grotest.py recv <port> <connect_ip|-> | send <ip> <port> <df:0|1> <connected:0|1> <gso:0|1>
import socket, struct, sys, time
SOL_UDP, UDP_SEGMENT, UDP_GRO = 17, 103, 104
if sys.argv[1] == 'recv':
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    s.bind(('0.0.0.0', int(sys.argv[2])))
    if sys.argv[3] != '-': s.connect((sys.argv[3], 40000))
    s.setsockopt(SOL_UDP, UDP_GRO, 1); s.settimeout(1.5)
    reads = pkts = 0; sizes = {}
    try:
        while True:
            data, anc, flags, addr = s.recvmsg(65536, 64)
            seg = len(data)
            for lvl, typ, val in anc:
                if lvl == SOL_UDP and typ == UDP_GRO: seg = struct.unpack('i', val[:4])[0]
            n = (len(data) + seg - 1) // seg; reads += 1; pkts += n; sizes[n] = sizes.get(n, 0) + 1
    except socket.timeout: pass
    print(f'reads={reads} packets={pkts} packets-per-read histogram={dict(sorted(sizes.items()))}')
else:
    ip, port, df, conn, gso = sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('0.0.0.0', 40000))
    if df: s.setsockopt(socket.IPPROTO_IP, 10, 3)  # IP_MTU_DISCOVER = IP_PMTUDISC_PROBE
    if conn: s.connect((ip, port))
    if gso: s.setsockopt(SOL_UDP, UDP_SEGMENT, 1350)
    for i in range(200):
        if gso:
            buf = b'x' * (1350 * 16)
            s.send(buf) if conn else s.sendto(buf, (ip, port))
        else:
            for k in range(16): s.send(b'x' * 1350) if conn else s.sendto(b'x' * 1350, (ip, port))
        time.sleep(0.002)
