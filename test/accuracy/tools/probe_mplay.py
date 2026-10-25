#!/usr/bin/env python3
"""Per-segment MPlayMain costs from an mgba_probe raw log recorded with the
ELF's nm listing as the symbol file (internal labels + registers).

    probe_mplay.py [--player BGM|SE1|SE2|SE3|<hex addr>] build/probe/groudon.txt [more logs ...] [frame ...]

With frame numbers: prints the breakpoint sequence of the selected player's
(default BGM) MPlayMain run for those engine frames (cycle relative to the MPlayMain
entry, delta from the previous breakpoint, label, registers).  Always:
a table of (from label -> to label) deltas with min/median/max and count,
over every frame, so each segment of the tick / command / apply loops is a
measured number.  r0 at a label is usually the value the preceding compare
loaded (track flags, gate time, wait), so the table is also keyed on it
for the few labels where that matters.
"""
import collections
import re
import sys

F1 = 124773
CPF = 280896
L160 = 160 * 1232
BGM = 0x03002008
# gMPlayInfo_* of the 2026-10-24 ROM (test/accuracy/rom/build/accuracy_test.map)
PLAYERS = {"BGM": 0x03002008, "SE1": 0x030021D8, "SE2": 0x03002218, "SE3": 0x030023DC}
LINE = re.compile(r"^\s*(\d+) f(\d+) v\s*(\d+) ([BW]) ([0-9A-F]{8}) ?(.*)$")
REG = re.compile(r"r0=([0-9A-F]+) r3=([0-9A-F]+) r4=([0-9A-F]+) r5=([0-9A-F]+) r7=([0-9A-F]+)")

# labels where r0 (or r4 chain) distinguishes paths worth separate stats
RAW_R0 = {"_081DD886", "_081DD9C8", "_081DDA62", "_081DD9F6", "_081DDA14"}
KEY_R0 = {"_081DD886", "_081DD8B4", "_081DD938", "_081DD9C8", "_081DDA62", "_081DD8EC", "_081DD8F6", "_081DD9F6", "_081DDA14", "_081DDBFA", "_081DDC14", "_081DDC18", "_081DDC24"}


def frame_of(t):
    return (t - F1) // CPF + 1, (t - F1) % CPF - L160


def parse(path):
    frames = collections.defaultdict(list)
    for line in open(path):
        m = LINE.match(line)
        if not m:
            continue
        t, _fc, _vc, kind, addr, rest = m.groups()
        t = int(t)
        k, rel = frame_of(t)
        if kind == "B":
            name = rest.split()[0] if rest else "?"
            rm = REG.search(rest)
            regs = tuple(int(x, 16) for x in rm.groups()) if rm else None
            frames[k].append((t, rel, name, regs, rest))
        else:
            a = int(addr, 16)
            if 0x04000060 <= a < 0x040000A0:
                frames[k].append((t, rel, "W:" + addr[-2:], None, rest))
    return frames


def bgm_run(ev):
    """Breakpoints of the selected player's MPlayMain (BGM unless --player):
    from the last MPlayMain entry before the first internal label with
    r7 == BGM up to CgbSound."""
    mp = [i for i, e in enumerate(ev) if e[2] == "MPlayMain"]
    cgb = [i for i, e in enumerate(ev) if e[2] == "CgbSound"]
    if not mp or not cgb:
        return None
    # the player's run is the MPlayMain entry whose following internal label carries r7 == BGM
    # (the selected player), or (paused: no labels) the one followed by FadeOutBody/_081DDA6C with it
    start = None
    for i in mp:
        for j in range(i + 1, cgb[0]):
            e = ev[j]
            if e[2] == "MPlayMain":
                break
            if e[3] is not None and e[3][4] == BGM:
                start = i
                break
        if start is not None:
            break
    if start is None:
        return None
    return ev[start:cgb[0] + 1]


def main(paths, show):
    seg = collections.defaultdict(list)
    for path in paths:
      frames = parse(path)
      for k in sorted(frames):
          run = bgm_run(sorted(frames[k], key=lambda e: e[0]))
          if not run:
              continue
          prev = None
          chainState = ""
          for e in run:
              t, rel, name, regs, rest = e
              cm0 = re.search(r"chan=(\S+)", rest)
              if cm0:
                  chainState = " chain=" + ("empty" if int(cm0.group(1), 16) == 0 else "nonempty")
              if name == "RealClearChain" and chainState:
                  rest = rest + chainState
                  e = (t, rel, name, regs, rest)
              if name == "_081DD938":
                  chainState = ""
              if prev is not None:
                  key = (prev[2], name)
                  r0 = prev[3][0] if prev[3] and prev[2] in KEY_R0 else None
                  if r0 is not None and prev[2] not in RAW_R0:
                      r0 = 1 if r0 else 0   # zero / nonzero is what the branch tests
                  fm = re.search(r"(flg=\S+(?: vt=\S+ lfodl=\S+)?|wait=\S+ lfos=\S+ mod=\S+ lfodlc=\S+|cgb\d st=\S+ prio=\S+|bytes=\d+)", prev[4])
                  if fm:
                      r0 = fm.group(1)   # handler path keyed on the track's flags / voice type
                      if r0.startswith("wait="):   # zero / nonzero of each field is what the code tests
                          r0 = " ".join(f"{k}={'0' if int(v, 16) == 0 else 'nz'}" for k, v in (x.split("=") for x in r0.split()))
                  cm = re.search(r"chan=(\S+)", prev[4])
                  if cm:
                      r0 = "chain=" + ("empty" if int(cm.group(1), 16) == 0 else "nonempty")
                  vm = re.search(r"flg=(\S+) vol=(\S+) volX=(\S+) pan=(\S+) panX=(\S+) modT=(\S+) modM=(\S+)", prev[4])
                  if vm:
                      flg, vol, volX, pan, panX, modT, modM = (int(x, 16) for x in vm.groups())
                      y = 2 * (pan - 256 if pan >= 128 else pan) + (panX - 256 if panX >= 128 else panX)
                      if modT == 2:
                          y += modM - 256 if modM >= 128 else modM
                      r0 = f"flg={flg:02X} modT={modT} modM={'0' if modM == 0 else 'nz'} yclamp={'lo' if y < -128 else ('hi' if y > 127 else 'no')}"
                  tm = re.search(r"trk=(\S+) prev=(\S+) next=(\S+)", prev[4])
                  if tm:
                      r0 = "trk=%s prev=%s next=%s" % tuple("0" if int(x, 16) == 0 else "nz" for x in tm.groups())
                      cm2 = re.search(r"chain=(\S+)", prev[4])
                      if cm2:
                          r0 += " chain=" + cm2.group(1)
                  seg[(key, r0)].append(t - prev[0])
              prev = e
          if k in show:
              t0 = run[0][0]
              print(f"== {path} frame {k}: MPlayMain(player) at +{run[0][1]} (line-160 relative), CgbSound at +{run[-1][1]}")
              prev = None
              for e in run:
                  d = e[0] - prev[0] if prev else 0
                  regs = "" if not e[3] else "r0=%X r3=%X r4=%X r5=%X r7=%X" % e[3]
                  if e[2] == "MPlayMain":
                      regs = e[4]
                  print(f"  {e[0]-t0:6d} {d:+5d}  {e[2]:<22s} {regs}")
                  prev = e
    print("\n== segment costs (from -> to [r0 at from]): n min med max")
    for (key, r0) in sorted(seg, key=lambda x: (x[0][0], x[0][1], str(x[1]))):
        v = sorted(seg[(key, r0)])
        r = "" if r0 is None else (f"[{r0}]" if isinstance(r0, str) else f"[r0={r0:X}]")
        spread = "" if v[0] == v[-1] else "   <- varies"
        print(f"  {key[0]:<22s} -> {key[1]:<22s} {r:<14s} n={len(v):4d} min={v[0]:5d} med={v[len(v)//2]:5d} max={v[-1]:5d}{spread}")


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--player" in args:
        i = args.index("--player")
        BGM = PLAYERS.get(args[i + 1].upper()) or int(args[i + 1], 16)
        del args[i:i + 2]
    paths = [a for a in args if not a.isdigit()]
    main(paths, set(int(a) for a in args if a.isdigit()))
