#!/usr/bin/env python3
"""Per-channel CgbSound pass costs from mgba_probe raw logs recorded with
the CgbSound_* breakpoints (see mgba_probe.c's header).

    probe_cgb.py build/probe/littleroot.txt [more logs ...] [-v]

For every CgbSound call, each channel's pass runs from its CgbSound_loop hit
to the next one (or to SoundMainRAM_Buffer after channel 4).  A pass is
summarized by its path (the CgbSound_* labels visited, with the state bytes
the probe printed at the loop head and at the envelope step) and the
registers it wrote; the table gives, per distinct path, the count and the
min/median/max of the offset of each write from the pass start and of the
pass length.  -v also prints every pass.
"""
import collections
import re
import sys

LINE = re.compile(r"^\s*(\d+) f(\d+) v\s*(\d+) ([BW]) ([0-9A-F]{8}) ?(.*)$")
REGS = {0x60: "NR10", 0x62: "NR11", 0x63: "NR12", 0x64: "NR13", 0x65: "NR14",
        0x68: "NR21", 0x69: "NR22", 0x6C: "NR23", 0x6D: "NR24",
        0x70: "NR30", 0x72: "NR31", 0x73: "NR32", 0x74: "NR33", 0x75: "NR34",
        0x78: "NR41", 0x79: "NR42", 0x7C: "NR43", 0x7D: "NR44",
        0x80: "NR50", 0x81: "NR51", 0x84: "NR52"}
for i in range(16):
    REGS[0x90 + i] = "WAVE"


# mgba_probe's return breakpoints (docs/ACCURACY.md, "SoundMain cost model"):
# not part of a CgbSound pass, which still ends at SoundMainRAM_Buffer
EXITS = ("SoundMain_cgbret", "SoundMain_return", "m4aSoundMain_return")


def passes(path):
    ev = []
    for line in open(path):
        m = LINE.match(line)
        if not m:
            continue
        t, _fc, _vc, kind, addr, rest = m.groups()
        t = int(t)
        if kind == "B":
            name = rest.split()[0] if rest else "?"
            if name in EXITS:
                continue
            ev.append((t, "B", name, rest))
        else:
            a = int(addr, 16)
            if 0x04000060 <= a < 0x040000A0:
                ev.append((t, "W", REGS.get(a & 0xFF, "%02X" % (a & 0xFF)), rest))
    out = []
    cur = None
    for t, kind, name, rest in ev:
        if kind == "B" and name == "CgbSound_loop":
            if cur:
                cur["end"] = t
                out.append(cur)
            m = re.search(r"ch(\d) st=(\S+) mod=(\S+) envc=(\S+) envv=(\S+)", rest)
            cur = {"t0": t, "ch": m.group(1), "st": m.group(2), "mod": m.group(3), "envc": m.group(4),
                   "labels": [], "writes": [], "end": None}
        elif cur and kind == "B" and (name.startswith("CgbSound_") or name in ("CgbModVol", "CgbOscOff")):
            tag = name.replace("CgbSound_", "")
            if tag == "step":
                m = re.search(r"st=(\S+) mod=(\S+) envc=(\S+)", rest)
                tag += "(st=%s envc=%s)" % (m.group(1), m.group(3))
            cur["labels"].append((t - cur["t0"], tag))
        elif cur and kind == "B" and name in ("SoundMainRAM_Buffer",):
            cur["end"] = t
            out.append(cur)
            cur = None
        elif cur and kind == "W":
            cur["writes"].append((t - cur["t0"], name))
        elif cur and kind == "B" and name not in ("CgbModVol", "CgbOscOff"):
            # anything else (next frame's breakpoints) ends an unterminated pass
            cur = None
    return out


def main(paths, verbose):
    table = collections.defaultdict(list)
    for p in paths:
        for ps in passes(p):
            if ps["end"] is None:
                continue
            key = (ps["ch"], ps["st"], ps["mod"], "ch%s st=%s mod=%s envc=%s" % (ps["ch"], ps["st"], ps["mod"], ps["envc"]),
                   " ".join(l for _, l in ps["labels"]), " ".join(w for _, w in ps["writes"]))
            table[key].append((ps["end"] - ps["t0"], [o for o, _ in ps["writes"]], [o for o, _ in ps["labels"]]))
            if verbose:
                print(p.split("/")[-1], key[3], "|", " ".join("%s@%d" % (l, o) for o, l in ps["labels"]),
                      "|", " ".join("%s@%d" % (w, o) for o, w in ps["writes"]), "| len", ps["end"] - ps["t0"])

    def st(v):
        v = sorted(v)
        return "%d/%d/%d" % (v[0], v[len(v) // 2], v[-1]) if v[0] != v[-1] else "%d" % v[0]
    print("== CgbSound channel passes: state | path | writes -> n, pass length, write offsets")
    for key in sorted(table, key=lambda k: (k[0], k[3], k[4], k[5])):
        rows = table[key]
        lens = [r[0] for r in rows]
        nw = len(rows[0][1])
        offs = [st([r[1][i] for r in rows if len(r[1]) > i]) for i in range(nw)]
        print("  %-28s | %-60s | %-32s n=%4d len=%-14s writes@ %s" % (key[3], key[4][:60], key[5][:32], len(rows), st(lens), " ".join(offs)))


# ---------------------------------------------------------------------------
# Segment mode: deltas between consecutive CgbSound_* breakpoints / writes
# inside a channel pass, keyed on the channel and on the state the GBA code
# branches on: the status / modify / envelopeCounter bytes at the 'from'
# label (and at the 'to' label, which tells which transition the step took),
# and for CgbModVol the CgbPan class of its volumes (R: right/2 >= left,
# L: left/2 >= right, C: centre) and whether the goal clamps at 15.
#
#     probe_cgb.py --segments build/probe/*.txt      (add -v for every key)
# ---------------------------------------------------------------------------
STATE = re.compile(r"ch(\d) st=(\S+) mod=(\S+) envc=(\S+) envv=(\S+) goal=(\S+) iec=(\S+) n4=(\S+) swp=(\S+)")
MODVOL = re.compile(r"rv=(\S+) lv=(\S+) sus=(\S+)")


def pan_class(rv, lv):
    if rv >= lv:
        return "R" if rv // 2 >= lv else "C"
    return "L" if lv // 2 >= rv else "C"


def segments(paths, verbose):
    table = collections.defaultdict(list)
    for p in paths:
        prev = None
        ch = "?"
        state = None
        for line in open(p):
            m = LINE.match(line)
            if not m:
                continue
            t, _fc, _vc, kind, addr, rest = m.groups()
            t = int(t)
            if kind == "B":
                name = rest.split()[0] if rest else "?"
                if name in EXITS:
                    continue
                if not (name.startswith("CgbSound") or name in ("CgbModVol", "CgbOscOff", "SoundMainRAM_Buffer")):
                    prev = None
                    continue
                sm = STATE.search(rest)
                tag = ""
                newstate = state
                if sm:
                    ch = sm.group(1)
                    st, mod, envc = (int(x, 16) for x in sm.groups()[1:4])
                    newstate = "st=%02X mod=%02X envc=%s" % (st, mod, "0" if envc == 0 else "nz")
                    tag = newstate
                mm = MODVOL.search(rest)
                if mm:
                    rv, lv, sus = (int(x, 16) for x in mm.groups())
                    tag = "pan=%s clamp=%d" % (pan_class(rv, lv), 1 if (rv + lv) // 16 > 15 else 0)
                label = name
            else:
                a = int(addr, 16)
                if not (0x04000060 <= a < 0x040000A0):
                    continue
                label = "W:" + REGS.get(a & 0xFF, "%02X" % (a & 0xFF))
                if label in ("W:NR30", "W:NR10"):
                    label += "=" + rest.split("<-")[1].split()[0][-2:]
                tag = ""
                newstate = state
            if prev is not None:
                # the 'to' state only when the step changed it (a transition)
                ttag = tag if kind == "B" and sm and newstate != state else ""
                table[("ch" + ch, prev[0], prev[1], label, ttag)].append(t - prev[2])
            prev = (label, tag if kind == "B" else "", t)
            state = newstate

    def st(v):
        v = sorted(v)
        return "n=%5d %d" % (len(v), v[0]) if v[0] == v[-1] else "n=%5d %d/%d/%d  <- varies" % (len(v), v[0], v[len(v) // 2], v[-1])
    # merge keys whose cost does not depend on the state unless -v
    merged = collections.defaultdict(list)
    for (chn, fr, ftag, to, ttag), v in table.items():
        merged[(chn, fr, to)].append((ftag, ttag, v))
    for key in sorted(merged):
        groups = merged[key]
        allv = [x for _, _, v in groups for x in v]
        if not verbose and min(allv) == max(allv):
            print("  %-4s %-20s -> %-22s %s" % (key[0], key[1], key[2], st(allv)))
            continue
        print("  %-4s %-20s -> %-22s" % (key[0], key[1], key[2]))
        for ftag, ttag, v in sorted(groups, key=lambda g: (g[0], g[1])):
            print("        from[%-24s] to[%-24s] %s" % (ftag, ttag, st(v)))


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a not in ("-v", "--segments")]
    if "--segments" in sys.argv:
        segments(args, "-v" in sys.argv)
    else:
        main(args, "-v" in sys.argv)


