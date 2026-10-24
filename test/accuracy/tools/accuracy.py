#!/usr/bin/env python3
"""poryaaaa accuracy harness: render with mgba and poryaaaa, compare, report.

Steps (all idempotent, cached under --build-dir):
  1. Overlay project: a directory that looks like a pokeemerald tree to
     poryaaaa's voicegroup loader (symlinks into --pokeemerald) plus the
     micro-test voicegroup, so poryaaaa and the ROM share one voicegroup file.
  2. References: mgba_render runs the test ROM once per test; results are
     cached by ROM hash + mgba version. --update-refs also stores the
     micro-test references in test/accuracy/refs/micro (the committed set,
     FLAC via the optional soundfile module), which is what runs without
     mgba_render.
  3. poryaaaa_render renders the same material.  With --input song (the
     default) it plays the compiled song (mid2agb's .s from the ROM build,
     byte-identical to what mgba plays) through the MPlayMain sequencer
     port; with --input midi it plays the MIDI with the mid2agb options from
     the .cfg mapped to flags (-V -> --song-volume, -R -> --reverb,
     -G -> voicegroup).
  4. metrics.compare on each pair; a table on stdout and a JSON report.

Typical use (after `make` in test/accuracy/rom and building mgba_render and
poryaaaa_render):
  tools/accuracy.py --pokeemerald ~/pokeemerald
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metrics  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ACC_DIR = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(ACC_DIR))
ROM_DIR = os.path.join(ACC_DIR, "rom")
MICRO_DIR = os.path.join(ROM_DIR, "micro")
COMMITTED_REFS = os.path.join(ACC_DIR, "refs", "micro")

GBA_CPU_HZ = 16777216
GBA_CYCLES_PER_FRAME = 280896
MICRO_SECONDS = 3.0


def parse_cfg(path):
    """midi.cfg / micro.cfg -> list of (name, options string), in file order."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, _, opts = line.partition(":")
            name = re.sub(r"\.mid$", "", name.strip())
            out.append((name, opts.strip()))
    return out


def mid2agb_opts(opts):
    """Pull voicegroup / volume / reverb / clock mode out of mid2agb options."""
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


def song_ids(pokeemerald):
    ids = {}
    with open(os.path.join(pokeemerald, "include", "constants", "songs.h")) as f:
        for line in f:
            m = re.match(r"#define\s+(\w+)\s+(\d+)", line)
            if m:
                ids[m.group(1)] = int(m.group(2))
    return ids


def build_tests(args):
    tests = []
    for i, (name, opts) in enumerate(parse_cfg(os.path.join(MICRO_DIR, "micro.cfg"))):
        vg, vol, rev, ext = mid2agb_opts(opts)
        tests.append(dict(name=name, group="micro", type="micro", id=i,
                          midi=os.path.join(MICRO_DIR, name + ".mid"),
                          song=os.path.join(args.rom_dir, "micro", name + ".s") if args.rom_dir else None,
                          voicegroup=vg, volume=vol, reverb=rev, extended=ext,
                          seconds=MICRO_SECONDS))
    if args.pokeemerald:
        midi_dir = os.path.join(args.pokeemerald, "sound", "songs", "midi")
        cfg = dict(parse_cfg(os.path.join(midi_dir, "midi.cfg")))
        ids = song_ids(args.pokeemerald)
        with open(os.path.join(ACC_DIR, "songs.json")) as f:
            songs = json.load(f)
        for s in songs:
            name = s["name"]
            const = name.upper()
            if const not in ids:
                print(f"warning: {const} not in songs.h, skipping", file=sys.stderr)
                continue
            vg, vol, rev, ext = mid2agb_opts(cfg[name])
            tests.append(dict(name=name, group="song", type="song", id=ids[const],
                              midi=os.path.join(midi_dir, name + ".mid"),
                              song=os.path.join(args.rom_dir, "songs", name + ".s") if args.rom_dir else None,
                              voicegroup=vg, volume=vol, reverb=rev, extended=ext,
                              seconds=float(s.get("seconds", 20.0))))
    if args.tests:
        wanted = set(args.tests.split(","))
        tests = [t for t in tests if t["name"] in wanted or t["group"] in wanted]
    return tests


def make_overlay(pokeemerald, root):
    """Fake pokeemerald tree for poryaaaa: symlinks plus the micro voicegroup."""
    sound = os.path.join(root, "sound")
    vgdir = os.path.join(sound, "voicegroups")
    os.makedirs(vgdir, exist_ok=True)

    def link(src, dst):
        if os.path.islink(dst) or os.path.exists(dst):
            if os.path.islink(dst) and os.readlink(dst) == src:
                return
            if os.path.islink(dst) or os.path.isfile(dst):
                os.remove(dst)
            else:
                shutil.rmtree(dst)
        os.symlink(src, dst)

    for entry in ("include", "asm", "data"):
        src = os.path.join(pokeemerald, entry)
        if os.path.exists(src):
            link(src, os.path.join(root, entry))
    src_sound = os.path.join(pokeemerald, "sound")
    for entry in os.listdir(src_sound):
        if entry == "voicegroups":
            continue
        link(os.path.join(src_sound, entry), os.path.join(sound, entry))
    src_vg = os.path.join(src_sound, "voicegroups")
    for entry in os.listdir(src_vg):
        link(os.path.join(src_vg, entry), os.path.join(vgdir, entry))
    link(os.path.join(ROM_DIR, "accuracy_micro.inc"), os.path.join(vgdir, "accuracy_micro.inc"))
    return root


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def frames_for(seconds):
    return int(math.ceil(seconds * GBA_CPU_HZ / GBA_CYCLES_PER_FRAME)) + 2


def run(cmd, log):
    with open(log, "w") as f:
        f.write(" ".join(cmd) + "\n")
        f.flush()
        r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    return r.returncode


def committed_ref(t):
    for ext in (".flac", ".wav"):
        p = os.path.join(COMMITTED_REFS, t["name"] + ext)
        if t["group"] == "micro" and os.path.exists(p):
            return p
    return None


def commit_ref(wav, name):
    """Store a micro-test reference in test/accuracy/refs/micro, as FLAC when possible."""
    os.makedirs(COMMITTED_REFS, exist_ok=True)
    for ext in (".flac", ".wav"):
        old = os.path.join(COMMITTED_REFS, name + ext)
        if os.path.exists(old):
            os.remove(old)
    try:
        import soundfile
        data, rate = soundfile.read(wav, dtype="int16", always_2d=True)
        soundfile.write(os.path.join(COMMITTED_REFS, name + ".flac"), data, rate, format="FLAC", subtype="PCM_16")
    except ImportError:
        shutil.copyfile(wav, os.path.join(COMMITTED_REFS, name + ".wav"))


def render_reference(args, t, ref_dir):
    wav = os.path.join(ref_dir, t["name"] + ".wav")
    if os.path.exists(wav) and not args.force:
        return wav
    if args.mgba_render is None:
        committed = committed_ref(t)
        if committed:
            return committed
        raise SystemExit(f"no mgba_render and no committed reference for {t['name']}")
    cmd = [args.mgba_render, args.rom, wav, "--frames", str(frames_for(t["seconds"])),
           "--rate", str(args.rate), "--type", t["type"], "--id", str(t["id"])]
    if run(cmd, wav + ".log") != 0:
        raise SystemExit(f"mgba_render failed for {t['name']}, see {wav}.log")
    return wav


def render_poryaaaa(args, t, overlay, out_dir):
    wav = os.path.join(out_dir, t["name"] + ".wav")
    common = ["--output", wav, "--sample-rate", str(args.rate),
              "--total-duration-seconds", str(t["seconds"]), "--fadeout", "0", "--tail", "0"]
    if args.input == "song":
        if not t["song"] or not os.path.exists(t["song"]):
            raise SystemExit(f"compiled song {t['song']} not found; build the ROM or use --input midi")
        # Volume, reverb and voicegroup come from the .s header.
        cmd = [args.poryaaaa_render, overlay, "--song", t["song"]] + common
    else:
        cmd = [args.poryaaaa_render, overlay, t["voicegroup"], "--midi", t["midi"]] + common
        cmd += ["--song-volume", str(t["volume"]), "--reverb", str(t["reverb"])]
        if t["extended"]:
            cmd.append("--extended-clocks")
    cmd += args.poryaaaa_args
    if run(cmd, wav + ".log") != 0:
        raise SystemExit(f"poryaaaa_render failed for {t['name']}, see {wav}.log")
    return wav


def fmt(v, width=7):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return " " * (width - 1) + "-"
    if isinstance(v, float) and math.isinf(v):
        return ("%" + str(width) + "s") % ("-inf" if v < 0 else "inf")
    return ("%" + str(width) + ".1f") % v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pokeemerald", default=os.environ.get("POKEEMERALD", os.path.expanduser("~/pokeemerald")))
    ap.add_argument("--rom", default=None, help="accuracy test ROM (default: build/accuracy/rom or test/accuracy/rom/build)")
    ap.add_argument("--mgba-render", default=None, help="mgba_render binary (default: build/mgba_render; omit to use committed refs)")
    ap.add_argument("--poryaaaa-render", default=os.path.join(REPO, "build", "poryaaaa_render"))
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build", "accuracy"))
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--gate", type=float, default=-40.0, help="pass threshold in dB for the raw null depth (full and worst window)")
    ap.add_argument("--min-lag", type=float, default=-0.02, help="alignment search window start, seconds (test delayed by this much)")
    ap.add_argument("--max-lag", type=float, default=0.2, help="alignment search window end, seconds")
    ap.add_argument("--tests", default=None, help="comma-separated test names or groups (micro, song)")
    ap.add_argument("--input", choices=("song", "midi"), default="song",
                    help="what poryaaaa_render plays: the compiled .s from the ROM build (default) or the MIDI")
    ap.add_argument("--update-refs", action="store_true", help="store micro-test references in test/accuracy/refs/micro")
    ap.add_argument("--committed-refs", action="store_true", help="ignore mgba_render and compare against the committed micro-test references")
    ap.add_argument("--force", action="store_true", help="re-render references even if cached")
    ap.add_argument("--json", default=None, help="write the report here (default: <build-dir>/report.json)")
    ap.add_argument("--label", default="poryaaaa", help="label for the rendering under test in the report")
    ap.add_argument("poryaaaa_args", nargs="*", help="extra arguments passed to poryaaaa_render (after --)")
    args = ap.parse_args()

    if args.rom is None:
        for cand in (os.path.join(REPO, "build", "accuracy", "rom", "accuracy_test.gba"),
                     os.path.join(ROM_DIR, "build", "accuracy_test.gba")):
            if os.path.exists(cand):
                args.rom = cand
                break
    if args.committed_refs:
        args.mgba_render = None
    elif args.mgba_render is None:
        cand = os.path.join(REPO, "build", "mgba_render")
        if os.path.exists(cand):
            args.mgba_render = cand
    if args.mgba_render and not args.rom:
        raise SystemExit("test ROM not found; run make in test/accuracy/rom or pass --rom")
    args.rom_dir = os.path.dirname(args.rom) if args.rom else None
    if args.input == "song" and not args.rom_dir:
        raise SystemExit("--input song needs the ROM build (for the compiled .s files); pass --rom or use --input midi")
    if not os.path.isdir(args.pokeemerald):
        raise SystemExit(f"pokeemerald tree not found at {args.pokeemerald}")

    os.makedirs(args.build_dir, exist_ok=True)
    overlay = make_overlay(args.pokeemerald, os.path.join(args.build_dir, "project"))

    if args.mgba_render:
        ver = subprocess.run([args.mgba_render, "--version"], capture_output=True, text=True).stdout.strip() or "unknown"
        key = f"{sha256_file(args.rom)[:12]}-{ver.replace(' ', '_')}"
    else:
        ver, key = "committed", "committed"
    ref_dir = os.path.join(args.build_dir, "refs", key)
    out_dir = os.path.join(args.build_dir, "out")
    os.makedirs(ref_dir, exist_ok=True)
    os.makedirs(out_dir, exist_ok=True)

    tests = build_tests(args)
    results = []
    print(f"ROM: {args.rom}\nmgba: {ver}\nreferences: {ref_dir}\n")
    hdr = f"{'test':18s} {'lag ms':>7s} {'gain dB':>8s} | {'null':>7s} {'worst1s':>7s} | {'gm null':>7s} {'gm wrst':>7s} | {'ref':>6s} {'test':>6s} | {'drift':>7s} | gate {args.gate:.0f}"
    print(hdr)
    print("-" * len(hdr))
    for t in tests:
        ref = render_reference(args, t, ref_dir)
        if args.update_refs and t["group"] == "micro" and args.mgba_render:
            commit_ref(ref, t["name"])
        out = render_poryaaaa(args, t, overlay, out_dir)
        m = metrics.compare_files(ref, out, min_lag_s=args.min_lag, max_lag_s=args.max_lag)
        ok = (not math.isnan(m["null_db"])) and m["null_db"] <= args.gate and m["worst_db"] <= args.gate
        m.update(name=t["name"], group=t["group"], passed=bool(ok), ref=ref, test=out)
        results.append(m)
        print(f"{t['name']:18s} {fmt(m['lag_ms'])} {fmt(m['gain_db'], 8)} | {fmt(m['null_db'])} {fmt(m['worst_db'])} | "
              f"{fmt(m['gm_null_db'])} {fmt(m['gm_worst_db'])} | {fmt(m['ref_rms_dbfs'], 6)} {fmt(m['test_rms_dbfs'], 6)} | "
              f"{fmt(m.get('drift_ms_per_s'))} | {'PASS' if ok else 'FAIL'}")
        sys.stdout.flush()

    if args.update_refs:
        with open(os.path.join(COMMITTED_REFS, "manifest.json"), "w") as f:
            json.dump({"rom_sha256": sha256_file(args.rom), "mgba": ver, "rate": args.rate,
                       "seconds": MICRO_SECONDS}, f, indent=1)

    n_pass = sum(1 for r in results if r["passed"])
    print("\ncolumns: lag = alignment applied to the test render; gain = least-squares gain match (dB);"
          "\n  null / worst1s = RMS(ref - test) / RMS(ref) in dB over the file and the worst 1 s window (gate uses these);"
          "\n  gm = the same after gain matching; ref / test = RMS level dBFS; drift = lag slope, ms per second (tempo mismatch)")
    print(f"{n_pass}/{len(results)} pass at {args.gate:.0f} dB (raw null depth, full file and worst 1 s window)")
    report = {"rom": args.rom, "mgba": ver, "rate": args.rate, "gate_db": args.gate,
              "label": args.label, "input": args.input, "results": results}
    path = args.json or os.path.join(args.build_dir, "report.json")
    with open(path, "w") as f:
        json.dump(report, f, indent=1)
    print(f"report: {path}")
    return 0 if n_pass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
