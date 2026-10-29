#!/usr/bin/env python3
"""The MIDI-path sequencer estimate against the exact sequencer term and
the probe (docs/ACCURACY.md, "MIDI-path sequencer estimate").

    seq_estimate_diff.py [--probe DIR] [--out DIR] [--jobs N] [--quantize]
                         [--worst N] [--no-grid] [name ...]

For every probed test (<probe dir>/<name>.txt.frames, default build/probe)
that has a MIDI -- the micro-tests (test/accuracy/rom/micro/micro.cfg) and
the songs and sound effects of songs.json (build/pokeemerald-pristine's
sound/songs/midi/midi.cfg); cries and cry_over_* have none -- render it
twice with poryaaaa_render --midi --gba-tempo-grid --cost-log, as
accuracy.py --input midi does (-V -> --song-volume, -R -> --reverb, -G ->
the voicegroup, -X -> --extended-clocks, the song table's player):

  (a) with --cost-estimate: the published sequencer term is the estimate
      (the timing-only sequencer still places the PSG writes, so the audio
      and every other input to the cost model are those of (b));
  (b) without: the timing-only sequencer's exact term.

Then per test the seq column frame by frame, estimate - exact and
estimate - probe (the probe's seq = CGB - sm of .frames), from frame 3 (the
song's first tick) to the probe's last costed frame: mean signed, mean |d|,
max |d|, p50 / p90 / p99 of |d|, and p99 of |d| as a percentage of the
frame's total (the probe's tot).  Then the corpus summary per group (song,
se, micro) and overall.  --quantize renders both with --quantize (the
plugin's latch).  --worst N lists the N largest |d| frames per test (vs
exact).

--no-grid: a plugin's situation instead.  Without --gba-tempo-grid the
renders play at the MIDI's own tempo and no timing-only sequencer exists,
so each test is rendered twice with the estimate published: declared (the
renderer's m4a_engine_cost_song_start with the tracks that have notes, the
time signatures and the loop points: what porydaw does) and inferred
(--cost-inferred: none of them, the estimate infers the tracks from their
first notes and has no bar marks: a host that calls nothing).  Frames do
not line up with the game's, so per test the distributions of the seq
term (frames 3 to the probe's last) are compared with the probe's: mean,
p50 / p90 / p99, max, and W1 = the mean |difference| of the two sorted
series (the earth mover's distance of the per-frame values).

The cost logs stay in --out (default <probe dir>/seq-estimate[-q]).
The mixer, HQ mode and the PCM rate do not enter the seq term;
the probe dir only chooses the frames.
"""
import concurrent.futures
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cost_diff  # noqa: E402

ROOT = cost_diff.ROOT
ACC = os.path.join(ROOT, "test/accuracy")
MICRO_DIR = os.path.join(ACC, "rom/micro")
PRISTINE = os.path.join(ROOT, "build/pokeemerald-pristine")
MIDI_DIR = os.path.join(PRISTINE, "sound/songs/midi")
PROJECT = os.path.join(ROOT, "build/accuracy/project")
CPF = cost_diff.CPF
CPU_HZ = cost_diff.CPU_HZ


def parse_cfg(path):
    out = {}
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, _, opts = line.partition(":")
        out[re.sub(r"\.mid$", "", name.strip())] = opts.strip()
    return out


def mid2agb_opts(opts):
    vg, vol, rev, ext = None, 127, 0, False
    for tok in opts.split():
        if tok.startswith("-G"):
            vg = tok[2:].lstrip("_")
        elif tok.startswith("-V"):
            vol = int(tok[2:])
        elif tok.startswith("-R"):
            rev = int(tok[2:])
        elif tok == "-X":
            ext = True
    return vg, vol, rev, ext


def players():
    out = {}
    p = os.path.join(PRISTINE, "sound/song_table.inc")
    for line in open(p):
        m = re.match(r"\s*song\s+(\w+)\s*,\s*MUSIC_PLAYER_(\w+)\s*,", line)
        if m:
            out[m.group(1)] = m.group(2).lower()
    return out


def midi_test(name, micro, songs, pl):
    """(group, midi, opts, player) for a probe log name, or None (no MIDI)."""
    base = {"wave180": "wave"}.get(name, name)
    if base.startswith("cry_"):
        return None
    if base in micro:
        return "micro", os.path.join(MICRO_DIR, base + ".mid"), micro[base], None
    song = {"littleroot": "mus_littleroot", "groudon": "mus_weather_groudon"}.get(base)
    if song is None and base in ("credits", "mt_chimney", "rg_vs_trainer", "vs_trainer", "vs_wild"):
        song = "mus_" + base
    if song is None and base.startswith("se_"):
        song = base
    if song is None or song not in songs:
        return None
    return ("se" if song.startswith("se_") else "song"), os.path.join(MIDI_DIR, song + ".mid"), songs[song], pl.get(song)


def render(t, nframes, out, estimate, quantize, mode="grid"):
    group, midi, opts, player = t["midi"]
    vg, vol, rev, ext = mid2agb_opts(opts)
    seconds = (nframes + 3) * CPF / CPU_HZ
    tag = ("est" if estimate else "exact") if mode == "grid" else mode
    log = os.path.join(out, "%s.%s.cost" % (t["name"], tag))
    wav = os.path.join(out, "%s.%s.wav" % (t["name"], tag))
    if os.path.exists(log):
        os.remove(log)
    cmd = [cost_diff.RENDER, PROJECT, vg, "--midi", midi, "--song-volume", str(vol), "--reverb", str(rev),
           "--output", wav, "--sample-rate", "48000",
           "--total-duration-seconds", "%.3f" % seconds, "--fadeout", "0", "--tail", "0", "--cost-log", log]
    if mode == "grid":
        cmd.append("--gba-tempo-grid")
    elif mode == "inferred":
        cmd.append("--cost-inferred")
    if ext:
        cmd.append("--extended-clocks")
    if player and player != "bgm":
        cmd += ["--player", player]
    if quantize:
        cmd.append("--quantize")
    if estimate and mode == "grid":
        cmd.append("--cost-estimate")
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if os.path.exists(wav):
        os.remove(wav)
    if r.returncode != 0 or not os.path.exists(log):
        print("render failed: %s (%s): %s" % (t["name"], tag, r.stderr.strip()[-300:]), file=sys.stderr)
        return None
    return log


def pct(vals, p):
    return cost_diff.pct(vals, p)


def stats(d, tot):
    a = [abs(x) for x in d]
    rel = [100.0 * abs(x) / t for x, t in zip(d, tot) if t > 0]
    n = len(d)
    return {"n": n, "mean": sum(d) / n if n else 0.0, "mabs": sum(a) / n if n else 0.0,
            "max": max(a, default=0), "p50": pct(a, 50), "p90": pct(a, 90), "p99": pct(a, 99),
            "rel99": pct(sorted(rel), 99) if rel else 0.0, "rel_max": max(rel, default=0.0)}


def fmt_row(name, s):
    return "%-22s %6d %8.1f %7.1f %6d %5d %5d %6d %6.2f%%" % (
        name, s["n"], s["mean"], s["mabs"], s["max"], s["p50"], s["p90"], s["p99"], s["rel99"])


HEAD = "%-22s %6s %8s %7s %6s %5s %5s %6s %7s" % ("test", "frames", "mean", "mean|d|", "max|d|", "p50", "p90",
                                                  "p99", "p99 %tot")


def dist(v):
    s = sorted(v)
    n = len(s)
    return {"n": n, "mean": sum(s) / n if n else 0.0, "p50": pct(s, 50), "p90": pct(s, 90),
            "p99": pct(s, 99), "max": s[-1] if s else 0}


def w1(a, b):
    """Mean |difference| of the sorted series, the shorter one stretched."""
    a, b = sorted(a), sorted(b)
    if not a or not b:
        return 0.0
    n = max(len(a), len(b))
    return sum(abs(a[i * len(a) // n] - b[i * len(b) // n]) for i in range(n)) / n


def no_grid(tests, out, jobs, quantize):
    jobsl = [(t, m) for t in tests for m in ("declared", "inferred")]
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        logs = list(ex.map(lambda j: render(j[0], max(j[0]["rows"]), out, True, quantize, j[1]), jobsl))
    logs = {(j[0]["name"], j[1]): l for j, l in zip(jobsl, logs)}
    print("MIDI-path sequencer estimate without the grid%s: seq distributions, frames 3.. (cycles)" % (
        " (--quantize)" if quantize else ""))
    print("%-18s %-9s %6s %8s %6s %6s %6s %6s %7s %7s" % ("test", "", "frames", "mean", "p50", "p90", "p99",
                                                       "max", "mean d", "W1"))
    groups = {}
    for t in tests:
        n = max(t["rows"])
        probe = [v["seq"] for k, v in sorted((k, cost_diff.probe_terms(r)) for k, r in t["rows"].items()
                                             if k >= 3) if v]
        dp = dist(probe)
        print("%-18s %-9s %6d %8.0f %6d %6d %6d %6d" % (t["name"], "probe", dp["n"], dp["mean"], dp["p50"],
                                                        dp["p90"], dp["p99"], dp["max"]))
        for m in ("declared", "inferred"):
            l = logs[(t["name"], m)]
            if not l:
                print("%-18s %-9s render failed" % ("", m))
                continue
            e = cost_diff.read_cost_log(l)
            est = [e[k]["seq"] for k in sorted(e) if 3 <= k <= n]
            de = dist(est)
            md = de["mean"] - dp["mean"]
            wd = w1(est, probe)
            print("%-18s %-9s %6d %8.0f %6d %6d %6d %6d %+7.1f %7.1f" % ("", m, de["n"], de["mean"], de["p50"],
                                                                      de["p90"], de["p99"], de["max"], md, wd))
            for grp in (t["midi"][0], "all"):
                g = groups.setdefault((grp, m), {"md": [], "w1": [], "rel": []})
                g["md"].append(md); g["w1"].append(wd); g["rel"].append(100.0 * md / dp["mean"] if dp["mean"] else 0)
    print("summary (per test: mean of the means' difference, of |its %|, of W1):")
    for grp in ("song", "se", "micro", "all"):
        for m in ("declared", "inferred"):
            if (grp, m) in groups:
                g = groups[(grp, m)]
                k = len(g["md"])
                print("  %-6s %-9s tests %2d  mean d %+7.1f  |mean d| %5.2f %%  W1 %6.1f" % (
                    grp, m, k, sum(g["md"]) / k, sum(abs(x) for x in g["rel"]) / k, sum(g["w1"]) / k))
    return 0


def main(argv):
    probe = os.path.join(ROOT, "build/probe"); out = None; jobs = os.cpu_count() or 4
    quantize = False; worst = 0; names = []; nogrid = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--probe": probe = argv[i + 1]; i += 2
        elif a == "--out": out = argv[i + 1]; i += 2
        elif a == "--jobs": jobs = int(argv[i + 1]); i += 2
        elif a == "--worst": worst = int(argv[i + 1]); i += 2
        elif a == "--quantize": quantize = True; i += 1
        elif a == "--no-grid": nogrid = True; i += 1
        elif a in ("-h", "--help"): print(__doc__); return 0
        else: names.append(a); i += 1
    out = out or os.path.join(probe, "seq-estimate" + ("-nogrid" if nogrid else "") + ("-q" if quantize else ""))
    os.makedirs(out, exist_ok=True)
    micro = parse_cfg(os.path.join(MICRO_DIR, "micro.cfg"))
    songs = parse_cfg(os.path.join(MIDI_DIR, "midi.cfg"))
    pl = players()
    tests = []
    for f in sorted(os.listdir(probe)):
        if not f.endswith(".txt.frames"):
            continue
        name = f[:-len(".txt.frames")]
        if names and name not in names:
            continue
        m = midi_test(name, micro, songs, pl)
        if m:
            tests.append({"name": name, "midi": m, "rows": cost_diff.read_frames(os.path.join(probe, f))})
    if nogrid:
        return no_grid(tests, out, jobs, quantize)
    jobsl = [(t, est) for t in tests for est in (True, False)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        logs = list(ex.map(lambda j: render(j[0], max(j[0]["rows"]), out, j[1], quantize), jobsl))
    logs = {(j[0]["name"], j[1]): l for j, l in zip(jobsl, logs)}

    print("MIDI-path sequencer estimate (%s%s), seq column, cycles; frames 3.. of %s" % (
        "--midi --gba-tempo-grid", " --quantize" if quantize else "", probe))
    groups = {}
    for which in ("exact", "probe"):
        print("\nestimate - %s:" % ("the exact sequencer (timing-only, same render)" if which == "exact"
                                    else "the probe (mgba, CgbSound entry - m4aSoundMain entry)"))
        print(HEAD)
        for t in tests:
            le, lx = logs[(t["name"], True)], logs[(t["name"], False)]
            if not le or not lx:
                print("%-22s render failed" % t["name"])
                continue
            est, exa = cost_diff.read_cost_log(le), cost_diff.read_cost_log(lx)
            probe_t = {k: v for k, v in ((k, cost_diff.probe_terms(r)) for k, r in t["rows"].items()) if v}
            ref = exa if which == "exact" else {k: v for k, v in probe_t.items()}
            frames = sorted(k for k in est if k >= 3 and k in ref and k in probe_t)
            d = [est[k]["seq"] - ref[k]["seq"] for k in frames]
            tot = [probe_t[k]["total"] for k in frames]
            s = stats(d, tot)
            print(fmt_row(t["name"], s))
            g = groups.setdefault((which, t["midi"][0]), {"d": [], "tot": []})
            g["d"] += d; g["tot"] += tot
            a = groups.setdefault((which, "all"), {"d": [], "tot": []})
            a["d"] += d; a["tot"] += tot
            if worst and which == "exact":
                w = sorted(frames, key=lambda k: -abs(est[k]["seq"] - exa[k]["seq"]))[:worst]
                for k in w:
                    print("    f%-5d est %6d exact %6d (%+d)" % (k, est[k]["seq"], exa[k]["seq"],
                                                               est[k]["seq"] - exa[k]["seq"]))
        print("summary:")
        for grp in ("song", "se", "micro", "all"):
            if (which, grp) in groups:
                g = groups[(which, grp)]
                print(fmt_row("  " + grp, stats(g["d"], g["tot"])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
