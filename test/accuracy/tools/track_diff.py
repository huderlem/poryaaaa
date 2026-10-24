#!/usr/bin/env python3
"""Compare the per-track end positions of MPlayMain's tick (the probe's
_081DD998 breakpoints) with the sequencer trace's "  t<i> <cycles>" lines,
frame by frame; report the first track whose position differs and, after
the tick, the CgbSound entry difference.

    track_diff.py build/probe/<song>.txt build/probe/<song>.seq.log [max_lines]
"""
import re
import sys

F1 = 124773
CPF = 280896
L160 = 160 * 1232


def main(probe, seqlog, limit=20):
    mg = {}
    entry = {}
    for line in open(probe):
        m = re.match(r"^\s*(\d+) f\d+ v\s*\d+ B [0-9A-F]{8} (_081DD998|CgbSound)\b", line)
        if m:
            t = int(m.group(1))
            k = (t - F1) // CPF + 1
            rel = (t - F1) % CPF - L160
            if m.group(2) == "CgbSound":
                entry[k] = rel
            else:
                mg.setdefault(k, []).append(rel)
    en = {}
    enEntry = {}
    cur = []
    for line in open(seqlog):
        f = line.split()
        if line.startswith(" "):
            if f[0].startswith("t"):
                cur.append((int(f[0][1:]), int(f[1])))
        else:
            k = int(f[0])
            en[k] = cur
            enEntry[k] = int(f[1])
            cur = []
    shown = bad = 0
    for k in sorted(en):
        if k not in mg or k not in entry:
            continue
        e = [c for _, c in en[k]]
        p = mg[k]
        if len(e) != len(p):
            bad += 1
            if shown < limit:
                print(f"f{k}: track count differs: probe {len(p)} engine {len(e)}")
                shown += 1
            continue
        first = None
        for i, (pc, ec) in enumerate(zip(p, e)):
            if pc != ec:
                first = (i, pc, ec)
                break
        d = enEntry.get(k, 0) - entry[k]
        if first or d:
            bad += 1
            if shown < limit:
                if first:
                    i, pc, ec = first
                    print(f"f{k}: track {i % 10} (tick {i // 10}) ends at {pc} in mgba, {ec} in the engine (d={ec - pc:+d}); CgbSound entry d={d:+d}")
                else:
                    print(f"f{k}: all track ends match; CgbSound entry d={d:+d} (apply pass / tail)")
                shown += 1
    print(f"{len(en)} frames, {bad} with a divergence")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 20)
