#!/usr/bin/env python3
"""Markdown tables from a bench/wan.sh run directory (timed runs only; medians of per-run values)."""
import re
import statistics
import sys
from pathlib import Path

FIELD = re.compile(r"(\w+)=([0-9.:]+)")
NAMES = {"embrquic": "embr QUIC, `SEND_ZC`", "embrquicsm": "embr QUIC, `sendmsg`",
         "embrquicgso": "embr QUIC, `SEND_ZC` + GSO", "embrquicsmgso": "embr QUIC, `sendmsg` + GSO",
         "embrtcp": "embr TCP", "nc": "nc", "scp": "scp"}


def med(values, digits=2):
    return f"{statistics.median(values):.{digits}f}" if values else "—"


def spread(values, digits=2):
    return f"{statistics.median(values):.{digits}f} [{min(values):.{digits}f}, {max(values):.{digits}f}]" if values else "—"


def results(path):
    rows = {}
    for line in path.read_text().splitlines():
        if line.startswith("run"):
            rows.setdefault(line.split()[0].split("_", 1)[1], []).append(
                {key: float(value) for key, value in FIELD.findall(line) if ":" not in value})
    return rows


def pushes(path, timed):
    good = []
    if path.exists():
        skip = False
        for line in path.read_text().splitlines():
            if "terminated by signal" in line:
                skip = True
                continue
            if skip:
                skip = False
                continue
            parts = line.split()
            if len(parts) == 4 and parts[3] == "0":
                good.append((float(parts[1]), float(parts[2])))
    return good[-timed:]


def egress(path, timed):
    lines = []
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            if line.startswith("[quic-egress]"):
                lines.append({key: float(value) for key, value in FIELD.findall(line)})
    return lines[-timed:]


def ingress(path, timed):
    lines = []
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            if line.startswith("[quic-ingress]"):
                lines.append({key: float(value) for key, value in FIELD.findall(line)})
    return lines[-timed:]


def main(run_dir):
    run = Path(run_dir)
    for name in ("default", "gro-off", "ack10", "slots128"):
        pass_dir = run / name
        if not (pass_dir / "results.txt").exists():
            continue
        rows = results(pass_dir / "results.txt")
        print(f"\n### {name}\n")
        print("| row | n | throughput, Gbps | wall, s | receiver user | receiver sys | receiver user+sys | receiver host | of which irq | datagrams per read |")
        print("|---|---|---|---|---|---|---|---|---|---|")
        for row, runs in rows.items():
            col = lambda key: [r[key] for r in runs if key in r]
            ing = ingress(pass_dir / f"results.txt.{row}.recv.log", len(runs))
            per_read = med([line["datagrams"] / line["reads"] for line in ing], 1) if ing else "—"
            print(f"| {NAMES[row]} | {len(runs)} | {spread(col('gbps'))} | {spread(col('sec'))} | {med(col('user'))} | {med(col('sys'))} | "
                  f"{med([r['user'] + r['sys'] for r in runs])} | {med(col('host_cpu'))} | {med(col('host_irq'))} | {per_read} |")
        print("\n| row | pushes | sender user | sender sys | sender user+sys [min, max] | sender host | of which irq | datagrams | sends | datagrams per send | copied | waits |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|")
        for row, runs in rows.items():
            push = pushes(pass_dir / "sender" / f"push.{row}.txt", len(runs))
            eg = egress(pass_dir / "sender" / f"push.{row}.log", len(runs))
            col = lambda key: [r[key] for r in runs if key in r]
            cells = ["—"] * 5
            if eg:
                cells = [med([e["datagrams"] for e in eg], 0), med([e["sends"] for e in eg], 0),
                         med([e["datagrams"] / e["sends"] for e in eg], 1),
                         (f"{sum(e.get('copied', 0) for e in eg):.0f} of {sum(e.get('notifs', 0) for e in eg):.0f}" if "notifs" in eg[0] else "all"),
                         med([e.get("waits", 0) for e in eg], 0)]
            print(f"| {NAMES[row]} | {len(push)} | {med([u for u, _ in push])} | {med([s for _, s in push])} | "
                  f"{spread([u + s for u, s in push]) if push else '—'} | {med(col('sender_cpu'))} | {med(col('sender_irq'))} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main(sys.argv[1])
