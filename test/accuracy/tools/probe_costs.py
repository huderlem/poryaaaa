#!/usr/bin/env python3
"""Summarize mgba_probe raw logs into per-command MPlayMain costs and
CgbSound write offsets.

    probe_costs.py build/probe/*.txt

For every engine frame (frame k >= 1 starts at cycle 124773 + (k-1)*280896)
the breakpoints between the first MPlayMain entry and the CgbSound entry are
walked in order; the cycle delta from one entry to the next is attributed to
the earlier symbol (its handler cost plus MPlayMain's dispatch overhead up to
the next event).  The last delta runs to CgbSound and so includes MPlayMain's
tail (VOLCHG/PITCHG pass, the remaining tracks, the idle players).

Inside CgbSound the deltas between consecutive PSG writes are grouped by
(previous register, next register) and the offset of each channel's first
write from the CgbSound entry (or from the previous channel's last write) is
tabulated.
"""
import collections
import re
import sys

FRAME1 = 124773
CPF = 280896
LINE160 = 160 * 1232

REGS = {0x60: "NR10", 0x62: "NR11", 0x63: "NR12", 0x64: "NR13", 0x65: "NR14",
        0x68: "NR21", 0x69: "NR22", 0x6C: "NR23", 0x6D: "NR24",
        0x70: "NR30", 0x72: "NR31", 0x73: "NR32", 0x74: "NR33", 0x75: "NR34",
        0x78: "NR41", 0x79: "NR42", 0x7C: "NR43", 0x7D: "NR44",
        0x80: "NR50", 0x81: "NR51", 0x84: "NR52"}
for i in range(16):
    REGS[0x90 + i] = "WAVE%d" % i

LINE = re.compile(r"^\s*(-?\d+) f(\d+) v\s*(\d+) ([BW]) ([0-9A-F]{8})\s*(.*)$")


def frame_of(t):
    if t < FRAME1:
        return 0, t
    k = (t - FRAME1) // CPF + 1
    return k, (t - FRAME1) % CPF - LINE160


def parse(path):
    frames = collections.defaultdict(list)  # k -> [(rel, kind, name, extra)]
    with open(path) as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            t, _fc, _vc, kind, addr, rest = m.groups()
            t = int(t)
            k, rel = frame_of(t)
            if kind == "B":
                name = rest.split()[0] if rest else "?"
                frames[k].append((rel, "B", name, rest))
            else:
                a = int(addr, 16)
                if 0x04000060 <= a < 0x040000A0:
                    v = int(rest.split("<-")[1].split()[0], 16) & 0xFF
                    frames[k].append((rel, "W", REGS.get(a & 0xFF, "%02X" % (a & 0xFF)), v))
    return frames


def main(paths):
    cmd_costs = collections.defaultdict(list)    # (sym, next_sym) -> deltas
    tail_costs = collections.defaultdict(list)   # key -> MPlayMain tail (last ply -> CgbSound)
    write_gaps = collections.defaultdict(list)   # (prev_reg, reg) -> deltas
    first_write = collections.defaultdict(list)  # (reg, nwrites_before) -> offset from CgbSound entry
    cgb_len = collections.defaultdict(list)      # (nwrites) -> CgbSound entry -> SoundMainRAM
    entries = collections.defaultdict(list)      # (tracks exist, tick?, notes, ply) -> CgbSound entry

    for path in paths:
        frames = parse(path)
        for k in sorted(frames):
            ev = sorted(frames[k], key=lambda e: e[0])
            mp = [e for e in ev if e[1] == "B" and e[2] == "MPlayMain"]
            cgb = [e for e in ev if e[1] == "B" and e[2] == "CgbSound"]
            ram = [e for e in ev if e[1] == "B" and e[2] == "SoundMainRAM_Buffer"]
            if not mp or not cgb:
                continue
            mp0 = mp[0]
            m = re.search(r"exist=(\d+) start=(\d+) tempoC=(\d+)", mp0[3])
            exist, start, tempoC = (int(x) for x in m.groups()) if m else (-1, -1, -1)
            # breakpoints between MPlayMain and CgbSound
            seq = [e for e in ev if e[1] == "B" and mp0[0] <= e[0] < cgb[0][0]]
            plys = [e for e in seq if e[2].startswith("ply_")]
            others = [e for e in seq if not e[2].startswith("ply_") and e[2] not in ("MPlayMain",)]
            notes = sum(1 for e in plys if e[2] == "ply_note")
            key = (exist, start, "tick" if (tempoC + 150) >= 150 and plys else "?", notes, len(plys), tuple(sorted(collections.Counter(e[2] for e in seq if e[2] != "MPlayMain").items())))
            entries[key].append((path.split("/")[-1], k, cgb[0][0], tempoC))
            # command deltas
            chain = [e for e in seq if e[2] != "MPlayMain" or e is mp0] + [cgb[0]]
            for a, b in zip(chain, chain[1:]):
                cmd_costs[(a[2], b[2])].append(b[0] - a[0])
            # CgbSound internals
            writes = [e for e in ev if e[1] == "W" and e[0] >= cgb[0][0]]
            if ram:
                cgb_len[len(writes)].append(ram[0][0] - cgb[0][0])
            prev = None
            for i, w in enumerate(writes):
                if prev is None:
                    first_write[(w[2], exist)].append(w[0] - cgb[0][0])
                else:
                    write_gaps[(prev[2], w[2])].append(w[0] - prev[0])
                prev = w

    def stats(v):
        v = sorted(v)
        return "n=%3d min=%5d med=%5d max=%5d" % (len(v), v[0], v[len(v) // 2], v[-1])

    print("== CgbSound entry (cycles from VBlank line-160 start) by frame work")
    for key in sorted(entries, key=lambda k: (k[0], k[3], k[4])):
        vals = entries[key]
        cyc = [v[2] for v in vals]
        print("  exist=%d start=%d notes=%d ply=%d %s: %s  e.g. %s f%d" % (key[0], key[1], key[3], key[4], stats(cyc), dict(key[5]) if key[5] else "{}", vals[0][0], vals[0][1]))
    print("\n== MPlayMain breakpoint-to-next deltas (cost of the first symbol + dispatch)")
    for key in sorted(cmd_costs):
        print("  %-22s -> %-22s %s" % (key[0], key[1], stats(cmd_costs[key])))
    print("\n== CgbSound length (entry -> SoundMainRAM) by number of PSG writes")
    for n in sorted(cgb_len):
        print("  writes=%2d %s" % (n, stats(cgb_len[n])))
    print("\n== first PSG write offset from CgbSound entry")
    for key in sorted(first_write):
        print("  %-6s exist=%d %s" % (key[0], key[1], stats(first_write[key])))
    print("\n== gaps between consecutive PSG writes")
    for key in sorted(write_gaps):
        print("  %-6s -> %-6s %s" % (key[0], key[1], stats(write_gaps[key])))


if __name__ == "__main__":
    main(sys.argv[1:])
