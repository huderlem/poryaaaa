#!/usr/bin/env python3
"""Compare two probe corpora (mgba_probe logs of the vanilla and the HQ-mixer
test ROM, docs/HQ-MIXER-ACCURACY.md step 2) test by test:

  probe_hq_compare.py <vanilla dir> <hq dir> [name ...]

For each log present in both: the boot IO writes before the first SoundMain
(register, value, cycle; any moved cycle is a boot-timeline change), the
per-frame MPlayMain / CgbSound / SoundMainRAM entry cycles and the PSG write
lists of the .frames files (the timing model carrying over), and, from the
HQ raw log, sp at SoundMainRAM entry, the mixer's cost (SoundMainRAM_Buffer
to C_downsampler_return) and the channels skipped by the stack-budget check
(C_mixing_setup straight to C_end_mixing).
"""
import os, re, sys
from collections import Counter

B_RE = re.compile(r"^\s*(\d+) f(\d+) v\s*(\d+) B [0-9A-F]{8} (\S+)(.*)$")
W_RE = re.compile(r"^\s*(\d+) f(\d+) v\s*(\d+) W ([0-9A-F]{8}) <- ([0-9A-F]{8})")


def raw(path):
    ev = []
    with open(path) as f:
        for line in f:
            m = B_RE.match(line)
            if m:
                ev.append((int(m.group(1)), "B", m.group(4), m.group(5).strip()))
                continue
            m = W_RE.match(line)
            if m:
                ev.append((int(m.group(1)), "W", m.group(4), m.group(5)))
    return ev


def boot_writes(ev):
    """IO writes (not FIFO) before the first SoundMain entry."""
    out = []
    for t, k, a, b in ev:
        if k == "B" and a == "SoundMain":
            break
        if k == "W" and a not in ("040000A0", "040000A4"):
            out.append((t, a, b))
    return out


def frames(path):
    rows = {}
    with open(path) as f:
        for line in f:
            m = re.match(r"^frame\s+(\d+) \| (.*)$", line)
            if m:
                rows[int(m.group(1))] = m.group(2)
    return rows


def frame_fields(s):
    m = re.match(r"sm\s+(-?\d+) SM\s+(-?\d+) MP\s+(-?\d+) CGB\s+(-?\d+) RAM\s+(-?\d+) \| vs\s+(-?\d+) \|(.*)\| c15\s*(-?\d+)->\s*(-?\d+) \|(.*)$", s)
    if not m:
        return None
    return {"sm": int(m.group(1)), "SM": int(m.group(2)), "MP": int(m.group(3)), "CGB": int(m.group(4)),
            "RAM": int(m.group(5)), "vs": int(m.group(6)), "mp": m.group(7).strip(), "c15": (m.group(8), m.group(9)),
            "writes": m.group(10).strip()}


def hq_mixer_stats(ev):
    sps = Counter(); costs = []; skipped = 0; setups = 0
    entry = None; last = None
    for t, k, a, b in ev:
        if k != "B":
            continue
        if a == "SoundMainRAM_Buffer":
            entry = t
            m = re.search(r"sp=([0-9A-F]{8})", b)
            if m:
                sps[m.group(1)] += 1
        elif a == "C_downsampler_return" and entry is not None:
            costs.append(t - entry); entry = None
        elif a == "C_mixing_setup":
            setups += 1; last = "setup"
        elif a.startswith("C_data_load") or a in ("C_setup_synth", "C_setup_fixed_freq_mixing"):
            last = "load"
        elif a == "C_end_mixing" and last == "setup":
            skipped += 1; last = None
        elif a == "C_mixing_epilogue":
            last = None
    return sps, costs, skipped, setups


def main():
    van, hq = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] or sorted(os.path.basename(p)[:-11] for p in os.listdir(hq) if p.endswith(".txt.frames"))
    print(f"{'test':34s} {'boot':>8s} {'frames':>6s} {'entry!=':>7s} {'psg!=':>6s} {'c15!=':>6s} | {'sp':>8s} {'mixer min/max':>14s} {'stack-skip':>10s}")
    for n in names:
        pv, ph = os.path.join(van, n + ".txt"), os.path.join(hq, n + ".txt")
        if not (os.path.exists(pv) and os.path.exists(ph) and os.path.exists(pv + ".frames") and os.path.exists(ph + ".frames")):
            print(f"{n:34s} (missing)"); continue
        ev_v, ev_h = raw(pv), raw(ph)
        bv, bh = boot_writes(ev_v), boot_writes(ev_h)
        boot = "same" if bv == bh else f"{sum(1 for x, y in zip(bv, bh) if x != y) + abs(len(bv) - len(bh))} moved"
        fv, fh = frames(pv + ".frames"), frames(ph + ".frames")
        common = sorted(set(fv) & set(fh))
        entry_diff = psg_diff = c15_diff = 0
        first = None
        for k in common:
            a, b = frame_fields(fv[k]), frame_fields(fh[k])
            if a is None or b is None:
                continue
            if (a["sm"], a["SM"], a["MP"], a["CGB"], a["vs"], a["mp"]) != (b["sm"], b["SM"], b["MP"], b["CGB"], b["vs"], b["mp"]):
                entry_diff += 1; first = first or k
            if a["writes"] != b["writes"]:
                psg_diff += 1; first = first or k
            if a["c15"] != b["c15"]:
                c15_diff += 1
        sps, costs, skipped, setups = hq_mixer_stats(ev_h)
        sp = "/".join(sorted(sps)) if sps else "-"
        cost = f"{min(costs)}/{max(costs)}" if costs else "-"
        note = f"  first differing frame {first}" if first else ""
        print(f"{n:34s} {boot:>8s} {len(common):6d} {entry_diff:7d} {psg_diff:6d} {c15_diff:6d} | {sp:>8s} {cost:>14s} {skipped:5d}/{setups:<4d}{note}")


if __name__ == "__main__":
    main()
