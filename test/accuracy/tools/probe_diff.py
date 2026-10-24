#!/usr/bin/env python3
"""Diff the PSG register writes of an mgba_probe raw log against a
poryaaaa_render --psg-log, write by write, with cycles relative to the
start of VBlank line 160 of the engine frame.

    probe_diff.py build/probe/wave.txt build/probe/wave.engine.log [max_lines] [threshold_cycles]
"""
import re
import sys

F1 = 124773
CPF = 280896
L160 = 160 * 1232


def frame(t):
    return (t - F1) // CPF + 1, (t - F1) % CPF - L160


def main(probe, englog, limit=60, thresh=8):
    mg = []
    for line in open(probe):
        m = re.match(r'^\s*(\d+) f\d+ v\s*\d+ W (0400[0-9A-F]{4}) <- ([0-9A-F]+)', line)
        if m and 0x04000060 <= int(m.group(2), 16) < 0x040000A0:
            mg.append((int(m.group(1)), int(m.group(2), 16) & 0xFF, int(m.group(3), 16) & 0xFF))
    en = []
    for line in open(englog):
        t, r, v = line.split()
        en.append((int(t), int(r, 16), int(v, 16)))
    start = F1 + CPF * 2   # from frame 3: skip the boot writes
    mg = [x for x in mg if x[0] >= start]
    en = [x for x in en if x[0] >= start]
    print(f"mgba {len(mg)} writes, engine {len(en)} writes (from frame 3)")
    n = min(len(mg), len(en))
    worst = 0
    shown = 0
    for i in range(n):
        a, b = mg[i], en[i]
        ka, ra = frame(a[0])
        kb, rb = frame(b[0])
        d = b[0] - a[0]
        worst = max(worst, abs(d))
        flag = '' if (a[1] == b[1] and a[2] == b[2] and ka == kb) else '  <<< differs'
        if shown < limit and (abs(d) > thresh or flag):
            print(f"  f{ka:3d} mgba {a[1]:02X}={a[2]:02X} @{ra:6d}   engine f{kb:3d} {b[1]:02X}={b[2]:02X} @{rb:6d}  d={d:+6d}{flag}")
            shown += 1
    print(f"max |d| over the first {n} writes: {worst} cycles")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 60,
         int(sys.argv[4]) if len(sys.argv) > 4 else 8)
