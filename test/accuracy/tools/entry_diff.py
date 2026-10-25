#!/usr/bin/env python3
"""Compare poryaaaa_render --seq-log (frame, modeled CgbSound entry) with an
mgba_probe .frames file, frame by frame.

    entry_diff.py build/probe/littleroot.txt.frames build/probe/littleroot.seq.log [max_lines]
"""
import re
import sys


def main(frames, seqlog, limit=40):
    mg = {}
    for line in open(frames):
        m = re.match(r"frame\s+(\d+) \|.*?CGB\s+(-?\d+)", line)
        if m and int(m.group(2)) >= 0:      # the probe's last, incomplete frame has no CgbSound entry (-999999)
            mg[int(m.group(1))] = int(m.group(2))
    en = {}
    for line in open(seqlog):
        if line.startswith(" "):
            continue            # per-track lines ("  t<i> <cycles>")
        k, c = line.split()
        en[int(k)] = int(c)
    common = sorted(k for k in en if k in mg and k >= 3)
    diffs = [(k, en[k] - mg[k]) for k in common]
    bad = [(k, d) for k, d in diffs if d != 0]
    print(f"{len(common)} frames compared, {len(bad)} differ; max |d| = {max((abs(d) for _, d in diffs), default=0)}")
    for k, d in bad[:limit]:
        print(f"  f{k:4d} engine {en[k]:6d} mgba {mg[k]:6d} d={d:+6d}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 40)
