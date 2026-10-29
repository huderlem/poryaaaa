#!/usr/bin/env python3
"""SoundMain cost: the probe's per-frame cycles (mgba_probe .frames, the RET /
EXIT / CR columns) against poryaaaa's cost model (docs/ACCURACY.md,
"SoundMain cost model").

    cost_diff.py --summarize <probe dir> [--render] [--hq] [--pcm-rate HZ] [name ...]
    cost_diff.py <probe .frames> <cost log> [max_lines]

--summarize: per probed test, the mixer cost per frame (mix = EXIT - RAM:
SoundMainRAM_Buffer entry to m4aSoundMain's return) against the frame's
DirectSound work, read from `<dir>/<name>.pcm.log` (poryaaaa_render
--pcm-log of the same test: the channels with a nonzero status after each
SoundMain are the frame's channel count n; a channel that ended inside the
frame's mixer is not counted).  Printed per test:
the frames, min / max / mean mix, the no-channel floor (mean over the frames
with n = 0 from frame 3 on: frame 2 is the idle SoundMain before the test
starts, the reverb not yet set), and a least-squares fit mix = a + b * n over
frames 3.. with its RMS and largest residual and b per output sample, and
the histogram n:frames.  Then the
corpus: mix min / max over all tests, tot (the whole m4aSoundMain) min / max
and every frame over the VBlank window (83776), the CgbSound tail (CR - the last
CgbSound_next) and SoundMain_4 -> SoundMainRAM (RAM - CR) histograms.
--render runs poryaaaa_render for every test that has no .pcm.log yet (the
test's song / cry arguments as claim_check.sh resolves them, --hq-mixer and
--pcm-mix-rate from the probe dir: HQ when its syms.nm has the C_ labels, the
rate from a -<hz> suffix; --hq / --pcm-rate override).  A test with no
pcm log (cry_third_*: poryaaaa has no third cry) gets min / max / mean only.

Frames: probe frame k starts at cycle 124773 + (k - 1) * 280896 (the first
SoundMain is frame 2); `--pcm-log`'s `f k` and `--cost-log`'s frame column
use the same counter (pcm-log `f 3` is probe frame 3).  A SoundMain longer
than VBlank keeps its returns on its own frame (RET / EXIT past the window).

Diff mode (step 2's acceptance): the cost log is poryaaaa_render --cost-log,
one line per engine frame `frame seq cgb mixer total`.  The probe's terms
are seq = CGB - sm (m4aSoundMain entry to CgbSound entry), cgb = RAM - CGB
(CgbSound to SoundMainRAM_Buffer, the tail and SoundMain_4 included), mixer
= EXIT - RAM and total = EXIT - sm; seq + cgb + mixer = total.  Per column:
the per-frame delta (engine - probe), max |d| and the 50 / 90 / 99th
percentiles of |d|; the gate: every frame's total within 2 % of the probe's
or 300 cycles, whichever is larger.  Frames missing on either side (the
probe's first and last frame, a skipped SoundMain) are counted, not gated.
Exit status 1 on FAIL.
"""
import collections
import json
import os
import re
import subprocess
import sys

CPU_HZ = 16777216
CPF = 280896
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
FRAME_RE = re.compile(r"^frame\s+(\d+) \| (.*)$")
FIELD_RE = re.compile(r"\b(sm|SM|MP|CGB|RAM|RET|EXIT|CR|mix|tot|tail|s2r)\s+(-?\d+)")
MISSING = -999999
VBLANK = 68 * 1232      # the VBlank window, 83776 cycles


def read_frames(path):
    """{frame: {field: value}} from a .frames file (the new columns optional)."""
    rows = {}
    for line in open(path):
        m = FRAME_RE.match(line)
        if not m:
            continue
        rows[int(m.group(1))] = {k: int(v) for k, v in FIELD_RE.findall(m.group(2).split("| vs")[0])}
    return rows


def probe_terms(r):
    """seq, cgb, mixer, total of a probe frame, or None when an end is missing."""
    need = ("sm", "CGB", "RAM", "EXIT")
    v = [r.get(k, MISSING) for k in need]
    # only a missing value (-999999) rejects a frame; the probe charges the
    # returns to their own SoundMain's frame, so an EXIT past the frame window
    # (a SoundMain longer than VBlank) is a real value, and the order check
    # only guards against a malformed line
    if any(x <= MISSING for x in v) or not v[0] <= v[1] <= v[2] <= v[3]:
        return None
    return {"seq": r["CGB"] - r["sm"], "cgb": r["RAM"] - r["CGB"],
            "mixer": r["EXIT"] - r["RAM"], "total": r["EXIT"] - r["sm"]}


def read_pcm_log(path):
    """{frame: DirectSound channels listed after that frame's SoundMain}.  A
    channel that ended inside the frame's mixer is not listed: a release
    reaching 0 or SF_STOP ends it before its mixing loop (the _081DCFB0
    store), the count path after a partial frame."""
    n = {}
    cur = None
    for line in open(path):
        if line.startswith("f "):
            cur = int(line.split()[1])
            n[cur] = 0
        elif cur is not None and line.strip():
            n[cur] += 1
    return n


# ---- the render of a probed test (claim_check.sh's mapping) ----

def song_players():
    p = os.path.expanduser("~/pokeemerald/sound/song_table.inc")
    out = {}
    if os.path.exists(p):
        for line in open(p):
            m = re.match(r"\s*song\s+(se_\w+)\s*,\s*MUSIC_PLAYER_(\w+)\s*,", line)
            if m:
                out[m.group(1)] = m.group(2).lower()
    return out


def render_args(name, romdir, songs, players):
    """poryaaaa_render's input arguments for a probe log name, or None."""
    base = re.sub(r"_p1$", "", name)
    base = {"cry_over_se_se": "cry_over_se", "cry_over_bgm_bgm": "cry_over_bgm"}.get(base, base)
    if base.startswith("cry_third_alone_"):   # Jynx then Wailord on VBlank 20: a second cry
        return ["--cry", "123", "--cry2", "381", "--cry2-start-frame", "20"]
    if base.startswith("cry_third_"):
        return None                            # no --cry3
    entry = next((s for s in songs if s["name"] == base), None)
    if base.startswith("cry_over_"):
        if not entry:
            return None
        song = entry.get("song", base)
        args = ["--song", os.path.join(romdir, "songs", song + ".s")]
        if song in players:
            args += ["--player", players[song]]
        return args + ["--cry2", str(entry["id2"]), "--cry2-start-frame", str(entry["start_frame2"])]
    if base.startswith("cry_"):
        if not entry or entry.get("type") != "cry":
            return None
        args = ["--cry", str(entry["id"])]
        for k in ("pitch", "pan", "volume", "length", "release", "chorus"):
            if k in entry:
                args += ["--cry-" + k, str(entry[k])]
        if "id2" in entry:
            args += ["--cry2", str(entry["id2"]), "--cry2-start-frame", str(entry["start_frame2"])]
        return args
    songname = {"littleroot": "mus_littleroot", "groudon": "mus_weather_groudon"}.get(base)
    if songname is None and base in ("credits", "mt_chimney", "rg_vs_trainer", "vs_trainer", "vs_wild"):
        songname = "mus_" + base
    if songname is None and (base.startswith("mus_") or base.startswith("se_")):
        songname = base
    if songname:
        args = ["--song", os.path.join(romdir, "songs", songname + ".s")]
        if songname in players:
            args += ["--player", players[songname]]
        return args
    src = os.path.join(romdir, "micro", base + ".s")
    return ["--song", src] if os.path.exists(src) else None


def render_pcm_log(name, nframes, outdir, hq, rate):
    romdir = os.path.join(ROOT, "build/accuracy/rom" + ("-hq" if hq else "") + ("-%d" % rate if rate != 13379 else ""))
    songs = json.load(open(os.path.join(ROOT, "test/accuracy/songs.json")))
    args = render_args(name, romdir, songs, song_players())
    if args is None:
        return False
    seconds = (nframes + 3) * CPF / CPU_HZ
    cmd = [os.path.join(ROOT, "build/poryaaaa_render"), os.path.join(ROOT, "build/accuracy/project")] + args
    if hq:
        cmd.append("--hq-mixer")
    if rate != 13379:
        cmd += ["--pcm-mix-rate", str(rate)]
    wav = os.path.join(outdir, name + ".cost.wav")
    cmd += ["--output", wav, "--sample-rate", "48000", "--total-duration-seconds", "%.3f" % seconds,
            "--fadeout", "0", "--tail", "0", "--pcm-log", os.path.join(outdir, name + ".pcm.log")]
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if os.path.exists(wav):
        os.remove(wav)
    if r.returncode != 0:
        print("render failed: %s: %s" % (name, r.stderr.strip()[-200:]), file=sys.stderr)
        return False
    return True


def fit(points):
    """Least squares y = a + b x; (a, b, rms residual, max |residual|)."""
    n = len(points)
    if n == 0:
        return None
    sx = sum(x for x, _ in points); sy = sum(y for _, y in points)
    sxx = sum(x * x for x, _ in points); sxy = sum(x * y for x, y in points)
    den = n * sxx - sx * sx
    if den == 0:
        a, b = sy / n, 0.0
    else:
        b = (n * sxy - sx * sy) / den
        a = (sy - b * sx) / n
    res = [y - (a + b * x) for x, y in points]
    rms = (sum(r * r for r in res) / n) ** 0.5
    return a, b, rms, max(abs(r) for r in res)


def summarize(argv):
    render = "--render" in argv
    argv = [a for a in argv if a != "--render"]
    hq = None; rate = None
    if "--hq" in argv:
        hq = True; argv.remove("--hq")
    if "--pcm-rate" in argv:
        i = argv.index("--pcm-rate"); rate = int(argv[i + 1]); del argv[i:i + 2]
    d = argv[0]; names = argv[1:]
    if hq is None:
        syms = os.path.join(d, "syms.nm")
        hq = os.path.exists(syms) and " C_downsampler\n" in open(syms).read()
    if rate is None:
        m = re.search(r"-(\d{4,5})/?$", d)
        rate = int(m.group(1)) if m else 13379
    spf = {13379: 224, 21024: 352}.get(rate, round(rate * CPF / CPU_HZ))
    tests = sorted(f[:-len(".txt.frames")] for f in os.listdir(d) if f.endswith(".txt.frames"))
    if names:
        tests = [t for t in tests if t in names]
    print("%s: %s mixer at %d Hz (%d samples per frame)" % (d, "HQ" if hq else "vanilla", rate, spf))
    print("%-34s %5s %6s %6s %7s %6s | %8s %7s %6s %6s %6s  n" % (
        "test", "frm", "min", "max", "mean", "floor", "a", "b", "b/smp", "rms", "maxres"))
    allmix = []; tails = collections.Counter(); s2rs = collections.Counter()
    for t in tests:
        rows = read_frames(os.path.join(d, t + ".txt.frames"))
        mix = {k: r["mix"] for k, r in rows.items() if r.get("mix", -1) >= 0}
        for r in rows.values():
            if r.get("tail", -1) >= 0:
                tails[r["tail"]] += 1
            if r.get("s2r", -1) >= 0:
                s2rs[r["s2r"]] += 1
        if not mix:
            print("%-34s no mix column (an older probe?)" % t)
            continue
        allmix += [(v, t, k) for k, v in mix.items()]
        pcm = os.path.join(d, t + ".pcm.log")
        if render and not os.path.exists(pcm):
            render_pcm_log(t, max(rows), d, hq, rate)
        vals = list(mix.values())
        line = "%-34s %5d %6d %6d %7.0f" % (t, len(vals), min(vals), max(vals), sum(vals) / len(vals))
        if os.path.exists(pcm):
            nch = read_pcm_log(pcm)
            # frame 2 is the idle SoundMain before the test starts (no reverb yet)
            pts = [(nch[k], v) for k, v in mix.items() if k in nch and k >= 3]
            fl = [v for n, v in pts if n == 0]
            floor = sum(fl) / len(fl) if fl else None
            f = fit(pts)
            if f is None:
                print(line + " %6s |  (no frames in the pcm log)" % "-")
                continue
            hist = collections.Counter(n for n, _ in pts)
            line += " %6s | %8.1f %7.1f %6.2f %6.1f %6.0f  %s" % (
                "%.0f" % floor if floor is not None else "-", f[0], f[1], f[1] / spf, f[2], f[3],
                " ".join("%d:%d" % (n, c) for n, c in sorted(hist.items())))
        else:
            line += " %6s |  (no pcm log)" % "-"
        print(line)
    if allmix:
        lo = min(allmix); hi = max(allmix)
        print("corpus: mix min %d (%s f%d) max %d (%s f%d)" % (lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]))
    tots = [(r["tot"], t, k) for t in tests for k, r in read_frames(os.path.join(d, t + ".txt.frames")).items()
            if r.get("tot", -1) >= 0]
    if tots:
        over = sorted((t, k, v) for v, t, k in tots if v > VBLANK)
        print("corpus: tot min %d max %d (%s f%d); %d frames over the VBlank window (%d)%s" % (
            min(tots)[0], max(tots)[0], max(tots)[1], max(tots)[2], len(over), VBLANK,
            ": " + ", ".join("%s f%d %d" % o for o in over) if over else ""))
    print("tail (CR - last CgbSound_next):", " ".join("%d:%d" % kv for kv in sorted(tails.items())))
    print("s2r  (RAM - CR):               ", " ".join("%d:%d" % kv for kv in sorted(s2rs.items())))


def pct(vals, p):
    if not vals:
        return 0
    s = sorted(vals)
    return s[min(len(s) - 1, int(round(p / 100 * (len(s) - 1))))]


def diff(frames_path, cost_path, limit=20):
    rows = read_frames(frames_path)
    probe = {k: probe_terms(r) for k, r in rows.items()}
    probe = {k: v for k, v in probe.items() if v}
    eng = {}
    for line in open(cost_path):
        p = line.split()
        if len(p) != 5 or not p[0].lstrip("-").isdigit():
            continue
        k, seq, cgb, mixer, total = (int(x) for x in p)
        eng[k] = {"seq": seq, "cgb": cgb, "mixer": mixer, "total": total}
    common = sorted(k for k in probe if k in eng)
    print("%d frames compared (%d probe-only, %d engine-only)" % (
        len(common), len(set(probe) - set(eng)), len(set(eng) - set(probe))))
    cols = ("seq", "cgb", "mixer", "total")
    for c in cols:
        d = [abs(eng[k][c] - probe[k][c]) for k in common]
        print("  %-6s max|d| %6d  p50 %5d  p90 %5d  p99 %5d" % (c, max(d, default=0), pct(d, 50), pct(d, 90), pct(d, 99)))
    bad = []
    for k in common:
        tol = max(0.02 * probe[k]["total"], 300)
        if abs(eng[k]["total"] - probe[k]["total"]) > tol:
            bad.append(k)
    for k in bad[:limit]:
        print("  f%-5d " % k + "  ".join("%s %d/%d (%+d)" % (c, eng[k][c], probe[k][c], eng[k][c] - probe[k][c]) for c in cols))
    ok = not bad and common
    print("%s: %d of %d frames outside max(2 %%, 300 cycles) on total" % ("PASS" if ok else "FAIL", len(bad), len(common)))
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--summarize":
        summarize(sys.argv[2:])
    elif len(sys.argv) >= 3:
        sys.exit(diff(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 20))
    else:
        print(__doc__)
        sys.exit(2)
