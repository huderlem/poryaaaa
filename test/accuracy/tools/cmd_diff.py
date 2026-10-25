#!/usr/bin/env python3
"""Line up the sequencer trace's per-command entries (poryaaaa_render
--seq-log: "  c<track> <cmd> <cycles>" at each handler entry) with the
probe's ply_* breakpoints, frame by frame, and report the first divergence
of each frame whose command positions differ.

    cmd_diff.py build/probe/<song>.txt build/probe/<song>.seq.log [max_frames]
"""
import re
import sys

F1 = 124773
CPF = 280896
L160 = 160 * 1232
NAMES = {0xB1: "ply_fine", 0xB2: "ply_goto", 0xB3: "ply_patt", 0xB4: "ply_pend", 0xB5: "ply_rept", 0xB9: "ply_memacc",
         0xBA: "ply_prio", 0xBB: "ply_tempo", 0xBC: "ply_keysh", 0xBD: "ply_voice", 0xBE: "ply_vol", 0xBF: "ply_pan",
         0xC0: "ply_bend", 0xC1: "ply_bendr", 0xC2: "ply_lfos", 0xC3: "ply_lfodl", 0xC4: "ply_mod", 0xC5: "ply_modt",
         0xC8: "ply_tune", 0xCC: "ply_port", 0xCD: "ply_xcmd", 0xCE: "ply_endtie"}


def main(probe, seqlog, limit=20):
    mg = {}
    for line in open(probe):
        m = re.match(r"^\s*(\d+) f\d+ v\s*\d+ B [0-9A-F]{8} (ply_\w+)", line)
        # the nm listing's local labels inside handlers are breakpoints too; only the handler entries count
        if m and m.group(2) not in ("ply_xiecv", "ply_xiecl", "ply_xrele", "ply_xwait", "ply_xcmd_0D", "ply_goto_1", "ply_pend_done", "ply_fine_loop", "ply_fine_ok",
                                    "ply_fine_done", "ply_patt_done", "ply_rept_1", "ply_rept_2"):
            t = int(m.group(1))
            k = (t - F1) // CPF + 1
            mg.setdefault(k, []).append((m.group(2), (t - F1) % CPF - L160))
    # ply_goto reached from ply_patt is a nested call: the probe logs both; the trace logs patt only
    en = {}
    cur = []
    for line in open(seqlog):
        if line.startswith(" "):
            f = line.split()
            if f[0].startswith("c"):          # "  c<track> <cmd> <cycles>" (the t<track> lines are the track ends)
                cur.append((int(f[1], 16), int(f[2])))
        else:
            k = int(line.split()[0])
            en[k] = cur
            cur = []
    shown = 0
    bad = 0
    for k in sorted(en):
        if k not in mg:
            continue
        e = [(("ply_note" if c >= 0xCF else NAMES.get(c, "ply_?")), cyc) for c, cyc in en[k] if c >= 0xB1]
        p = [x for x in mg[k] if not (x[0] == "ply_goto" and False)]
        # drop the probe's nested ply_goto after a ply_patt
        q = []
        for i, x in enumerate(p):
            if x[0] == "ply_goto" and i > 0 and p[i - 1][0] == "ply_patt":
                continue
            q.append(x)
        p = q
        if len(e) != len(p):
            bad += 1
            if shown < limit:
                print(f"f{k}: command count differs: probe {len(p)} engine {len(e)}")
                print("   probe ", p[:12])
                print("   engine", e[:12])
                shown += 1
            continue
        for (pn, pc), (en_, ec) in zip(p, e):
            if pn != en_ or pc != ec:
                bad += 1
                if shown < limit:
                    print(f"f{k}: first divergence at {pn}@{pc} vs engine {en_}@{ec} (d={ec - pc:+d})")
                    shown += 1
                break
    print(f"{len(en)} frames, {bad} with a command-position divergence")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 20)
