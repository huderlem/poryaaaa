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
     -G -> voicegroup); --midi-quantize adds --quantize (the MIDI event
     latch), and poryaaaa_render always gets --gba-tempo-grid (each MIDI tick
     on the ROM's tempoC / 150 frame, MIDI time 0 where the ROM starts the
     song, frame 3's SoundMain; --no-gba-tempo-grid leaves it out) and the
     start origin stated explicitly (--gba-start-delay, or --no-gba-start-delay
     with the runner's --no-gba-start-delay: MIDI time 0 at output sample 0).  A midi-input report (its "input" field says midi) is a
     separate baseline family, never compared against a song-input one.  A sound effect (songs.json "type": "se") is an
     m4aSongNumStart of its song id in the ROM and plays on the music player
     the song table names (sound/song_table.inc, the `ms` field: SE1 / SE2 /
     SE3 with 3 / 9 / 1 tracks), passed to poryaaaa_render as --player.
     A cry ("type": "cry", "id": the gCryTable index) is the ROM's StartCry
     (mgba_render --type cry --id N, PlayCryInternal's CRY_MODE_NORMAL
     defaults) and poryaaaa_render --cry N: the cry song template on cry
     player 0 with the cry's DPCM sample as its one voice.  Optional
     "pitch", "pan", "volume", "length", "release", "chorus" keys
     (SetPokemonCryPitch / Panpot / Volume / Length / Release / Chorus's
     arguments; a nonzero chorus makes the cry song two tracks) go to both
     renderers (mgba_render --pitch / --pan / --volume / --length / --release /
     --chorus, poryaaaa_render --cry-pitch / --cry-pan / --cry-volume /
     --cry-length / --cry-release / --cry-chorus).  "id2" / "start_frame2"
     start a second cry (gCryTable index, CRY_MODE_NORMAL's defaults) on
     VBlank start_frame2 (1..255) while the first plays, as mgba_render
     names them (--id2 N --start-frame2 F; AccParams bytes 12..14), and go
     to poryaaaa_render as --cry2 N --cry2-start-frame F (cry player 1,
     a second sequencer chained after cry 0's).  The same pair on an "se"
     or "song" entry (with "song": the name of the SE / song it plays, since
     such an entry is named after its probe log, cry_over_*) starts that cry
     over the sound effect or song
     (mgba_render's --type song run takes --id2 / --start-frame2 as well;
     poryaaaa_render --song ... [--player seN] --cry2 N --cry2-start-frame F,
     cry player 0 chained after the song's player).
     Cries have no MIDI, so --input midi skips them, and an SE or song with
     a cry over it the same way.
  4. metrics.compare on each pair; a table on stdout and a JSON report.

--mixer hq selects the HQ-mixer test ROM (build/accuracy/rom-hq, the same
sources built with MIXER=hq: ipatix's HQ mixer as the pret wiki installs it,
docs/HQ-MIXER-ACCURACY.md), its own reference cache and committed set
(test/accuracy/refs/micro-hq), passes --hq-mixer to poryaaaa_render and
writes "mixer": "hq" into the report: a separate baseline family
(baselines/<date>_hq-*.json), never compared against a vanilla one.
--engine-mixer vanilla|hq overrides what the engine is asked for (the
default follows --mixer); "engine_mixer" in the report records it.
The synth_* micro rows (the HQ mixer's synth paths, zero-size samples the
vanilla ROM plays as a click) run only with --mixer hq.

--pcm-rate <hz> (default 13379) selects a ROM built at another DirectSound
mix rate (make RATE=<hz>; build/accuracy/rom[-hq]-<hz>, CMake
accuracy_rom[_hq]_<hz>), passes --pcm-mix-rate <hz> to poryaaaa_render and
writes "pcm_rate" into the report: another baseline family
(baselines/<date>_[hq-]<hz>-*.json; the report is report[-hq]-<hz>.json).
Its micro references are not committed (refs/micro[-hq]-<hz> would be the
place); the run needs mgba_render.

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

# songs.json cry keys = the SetPokemonCry* setters StartCry takes: mgba_render --<key>, poryaaaa_render --cry-<key>.
CRY_PARAM_KEYS = ("pitch", "pan", "volume", "length", "release", "chorus")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metrics  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ACC_DIR = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(ACC_DIR))
ROM_DIR = os.path.join(ACC_DIR, "rom")
MICRO_DIR = os.path.join(ROM_DIR, "micro")
COMMITTED_REFS = os.path.join(ACC_DIR, "refs", "micro")          # --mixer hq: refs/micro-hq (set in main)

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


def song_players(pokeemerald):
    """song_table.inc: song name -> music player ("bgm", "se1", "se2", "se3"), its `ms` field."""
    players = {}
    with open(os.path.join(pokeemerald, "sound", "song_table.inc")) as f:
        for line in f:
            m = re.match(r"\s*song\s+(\w+)\s*,\s*MUSIC_PLAYER_(\w+)\s*,", line)
            if m:
                players[m.group(1)] = m.group(2).lower()
    return players


def song_ids(pokeemerald):
    ids = {}
    with open(os.path.join(pokeemerald, "include", "constants", "songs.h")) as f:
        for line in f:
            m = re.match(r"#define\s+(\w+)\s+(\d+)", line)
            if m:
                ids[m.group(1)] = int(m.group(2))
    return ids


def second_cry_ok(name, s):
    """The optional "id2" / "start_frame2" pair of a songs.json entry (a second cry
    while a cry plays, or a cry over an SE / song): both or neither, the ROM's
    u16 id2 (0..65535) and u8 startFrame2 (1..255; 0 means no second cry to the
    ROM but is refused by poryaaaa_render).  False, with a warning, on a violation."""
    if ("id2" in s) != ("start_frame2" in s):
        print(f"warning: {name}: a second cry needs both \"id2\" and \"start_frame2\", skipping", file=sys.stderr)
        return False
    if "id2" in s and not (0 <= int(s["id2"]) <= 0xFFFF and 1 <= int(s["start_frame2"]) <= 255):
        print(f"warning: {name}: \"id2\" takes 0..65535 and \"start_frame2\" 1..255, skipping", file=sys.stderr)
        return False
    return True


HQ_ONLY_PREFIX = "synth_"   # rows that only mean something on the HQ ROM (vanilla plays a zero-size sample as a click)


def build_tests(args):
    tests = []
    for i, (name, opts) in enumerate(parse_cfg(os.path.join(MICRO_DIR, "micro.cfg"))):
        if name.startswith(HQ_ONLY_PREFIX) and args.mixer != "hq":
            continue                     # the id (its line) is still taken: the ROM's table has every row
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
        players = song_players(args.pokeemerald)
        with open(os.path.join(ACC_DIR, "songs.json")) as f:
            songs = json.load(f)
        for s in songs:
            name = s["name"]
            kind = s.get("type", "song")       # "song": vanilla music; "se": a sound effect; "cry": a Pokemon cry
            if kind == "cry":
                # StartCry in the ROM (--type cry --id N [--pitch/--pan/--volume/--length/--release/--chorus]),
                # the cry song template on cry player 0 in poryaaaa_render
                # (--cry N [--cry-pitch/--cry-pan/--cry-volume/--cry-length/--cry-release/--cry-chorus]).
                if "id" not in s:
                    print(f"warning: {name}: a cry needs its gCryTable index as \"id\", skipping", file=sys.stderr)
                    continue
                if not second_cry_ok(name, s):
                    continue
                tests.append(dict(name=name, group="cry", type="cry", id=int(s["id"]), midi=None, song=None,
                                  voicegroup=None, volume=None, reverb=None, extended=False,
                                  seconds=float(s.get("seconds", 2.0)), player=None, cry=int(s["id"]),
                                  cry_params={k: int(s[k]) for k in CRY_PARAM_KEYS if k in s},
                                  cry2=int(s["id2"]) if "id2" in s else None,
                                  cry2_start_frame=int(s["start_frame2"]) if "id2" in s else None))
                continue
            song = s.get("song", name)         # the song it plays; the entry's name unless the row is a cry over it (cry_over_*)
            const = song.upper()
            if const not in ids:
                print(f"warning: {const} not in songs.h, skipping", file=sys.stderr)
                continue
            if kind not in ("song", "se"):
                print(f"warning: {name}: unknown type {kind!r}, skipping", file=sys.stderr)
                continue
            player = None
            if kind == "se":
                player = players.get(song)
                if player is None:
                    print(f"warning: {song} not in song_table.inc, skipping", file=sys.stderr)
                    continue
            if not second_cry_ok(name, s):
                continue
            vg, vol, rev, ext = mid2agb_opts(cfg[song])
            # Both play through m4aSongNumStart in the ROM (--type song); the
            # song table's player decides the track count and MPlayMain position.
            # "id2" / "start_frame2" here are a cry over the SE / song (StartTest
            # runs the second-cry block after its type switch): cry player 0
            # chained after the song's player in poryaaaa_render.
            tests.append(dict(name=name, group=kind, type="song", id=ids[const],
                              midi=os.path.join(midi_dir, song + ".mid"),
                              song=os.path.join(args.rom_dir, "songs", song + ".s") if args.rom_dir else None,
                              voicegroup=vg, volume=vol, reverb=rev, extended=ext,
                              seconds=float(s.get("seconds", 20.0 if kind == "song" else 3.0)),
                              player=player,
                              cry2=int(s["id2"]) if "id2" in s else None,
                              cry2_start_frame=int(s["start_frame2"]) if "id2" in s else None))
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
    # The micro voicegroup's synth descriptors, in the file the loader reads
    # inline set_synth_* definitions from (the pokeemerald-expansion layout).
    link(os.path.join(ROM_DIR, "accuracy_micro_synth.inc"), os.path.join(sound, "direct_sound_synth_data.inc"))
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
    for k, v in sorted(t.get("cry_params", {}).items()):      # StartCry's pitch / pan / volume / length / release / chorus
        cmd += ["--" + k, str(v)]
    if t.get("cry2") is not None:                             # a second cry at the defaults on VBlank start_frame2
        cmd += ["--id2", str(t["cry2"]), "--start-frame2", str(t["cry2_start_frame"])]
    if run(cmd, wav + ".log") != 0:
        raise SystemExit(f"mgba_render failed for {t['name']}, see {wav}.log")
    return wav


def render_poryaaaa(args, t, overlay, out_dir):
    wav = os.path.join(out_dir, t["name"] + ".wav")
    common = ["--output", wav, "--sample-rate", str(args.rate),
              "--total-duration-seconds", str(t["seconds"]), "--fadeout", "0", "--tail", "0"]
    if t.get("cry") is not None:
        if args.input != "song":
            print(f"{t['name']}: a cry has no MIDI; skipped with --input midi", file=sys.stderr)
            return None
        # The cry song template with gCryTable[id]'s sample, on cry player 0.
        cmd = [args.poryaaaa_render, overlay, "--cry", str(t["cry"])] + common
        for k, v in sorted(t.get("cry_params", {}).items()):
            cmd += ["--cry-" + k, str(v)]
        if t.get("cry2") is not None:                         # the second cry on cry player 1, chained after cry 0's
            cmd += ["--cry2", str(t["cry2"]), "--cry2-start-frame", str(t["cry2_start_frame"])]
    elif args.input == "song":
        if not t["song"] or not os.path.exists(t["song"]):
            raise SystemExit(f"compiled song {t['song']} not found; build the ROM or use --input midi")
        # Volume, reverb and voicegroup come from the .s header.
        cmd = [args.poryaaaa_render, overlay, "--song", t["song"]] + common
    else:
        if t.get("cry2") is not None:
            print(f"{t['name']}: a cry over the song has no MIDI; skipped with --input midi", file=sys.stderr)
            return None
        cmd = [args.poryaaaa_render, overlay, t["voicegroup"], "--midi", t["midi"]] + common
        cmd += ["--song-volume", str(t["volume"]), "--reverb", str(t["reverb"])]
        if t["extended"]:
            cmd.append("--extended-clocks")
        if args.midi_quantize:
            cmd.append("--quantize")         # the MIDI event latch (docs/ACCURACY.md, "Entry points and the frame latch")
        # The start origin, stated either way (the renderer's default follows --gba-tempo-grid):
        # MIDI time 0 where the ROM starts the song (slice 2a), or at output sample 0.
        cmd.append("--gba-start-delay" if args.gba_start_delay else "--no-gba-start-delay")
        if args.gba_tempo_grid:
            cmd.append("--gba-tempo-grid")   # each tick on the ROM's tempoC / 150 frame (slice 3a)
    if t.get("player"):
        cmd += ["--player", t["player"]]
    if t.get("cry") is None and t.get("cry2") is not None:
        # The cry over the SE / song: cry player 0 chained after the song's player (slice 9b-i).
        cmd += ["--cry2", str(t["cry2"]), "--cry2-start-frame", str(t["cry2_start_frame"])]
    if args.engine_mixer == "hq":
        cmd.append("--hq-mixer")             # the engine's HQ-mixer mode (docs/HQ-MIXER-ACCURACY.md, section 5)
    if args.pcm_rate != 13379:
        cmd += ["--pcm-mix-rate", str(args.pcm_rate)]   # the ROM's rate (step 11)
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
    ap.add_argument("--rom", default=None, help="accuracy test ROM (default: build/accuracy/rom[-hq] or test/accuracy/rom/build[-hq], by --mixer)")
    ap.add_argument("--mixer", choices=("vanilla", "hq"), default="vanilla",
                    help="which test ROM: vanilla pokeemerald or the ipatix HQ-mixer build (MIXER=hq); its own reference cache, committed set and baseline family")
    ap.add_argument("--engine-mixer", choices=("vanilla", "hq"), default=None,
                    help="what poryaaaa_render is asked for (hq adds --hq-mixer); default: the same as --mixer")
    ap.add_argument("--pcm-rate", type=int, default=13379,
                    help="the ROM's DirectSound mix rate in Hz (make RATE=<hz>: build/accuracy/rom[-hq]-<hz>); "
                         "poryaaaa_render gets --pcm-mix-rate <hz>; its own baseline family (default 13379)")
    ap.add_argument("--mgba-render", default=None, help="mgba_render binary (default: build/mgba_render; omit to use committed refs)")
    ap.add_argument("--poryaaaa-render", default=os.path.join(REPO, "build", "poryaaaa_render"))
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build", "accuracy"))
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--gate", type=float, default=-40.0, help="pass threshold in dB for the raw null depth (full and worst window)")
    ap.add_argument("--min-lag", type=float, default=-0.02, help="alignment search window start, seconds (test delayed by this much)")
    ap.add_argument("--max-lag", type=float, default=0.2, help="alignment search window end, seconds")
    ap.add_argument("--tests", default=None, help="comma-separated test names or groups (micro, song, se, cry)")
    ap.add_argument("--input", choices=("song", "midi"), default="song",
                    help="what poryaaaa_render plays: the compiled .s from the ROM build (default) or the MIDI")
    ap.add_argument("--midi-quantize", action="store_true",
                    help="with --input midi: pass --quantize to poryaaaa_render (events latched to the VBlank tick)")
    ap.add_argument("--no-gba-start-delay", dest="gba_start_delay", action="store_false",
                    help="with --input midi: pass --no-gba-start-delay to poryaaaa_render "
                         "(MIDI time 0 at output sample 0 instead of where the ROM starts the song; "
                         "the first frames' ticks then collapse onto frame 3)")
    ap.add_argument("--no-gba-tempo-grid", dest="gba_tempo_grid", action="store_false",
                    help="with --input midi: do not pass --gba-tempo-grid to poryaaaa_render "
                         "(the MIDI's exact tempo map instead of the frame MPlayMain's tempo accumulator plays each tick on)")
    ap.add_argument("--update-refs", action="store_true", help="store micro-test references in test/accuracy/refs/micro")
    ap.add_argument("--committed-refs", action="store_true", help="ignore mgba_render and compare against the committed micro-test references")
    ap.add_argument("--force", action="store_true", help="re-render references even if cached")
    ap.add_argument("--json", default=None, help="write the report here (default: <build-dir>/report.json, report-hq.json with --mixer hq)")
    ap.add_argument("--label", default="poryaaaa", help="label for the rendering under test in the report")
    ap.add_argument("poryaaaa_args", nargs="*", help="extra arguments passed to poryaaaa_render (after --)")
    args = ap.parse_args()

    global COMMITTED_REFS
    if args.engine_mixer is None:
        args.engine_mixer = args.mixer
    rate_suffix = "" if args.pcm_rate == 13379 else f"-{args.pcm_rate}"
    rom_sub = ("rom" if args.mixer == "vanilla" else "rom-hq") + rate_suffix
    if args.mixer == "hq" or rate_suffix:
        COMMITTED_REFS = os.path.join(ACC_DIR, "refs", ("micro-hq" if args.mixer == "hq" else "micro") + rate_suffix)
    if args.rom is None:
        for cand in (os.path.join(REPO, "build", "accuracy", rom_sub, "accuracy_test.gba"),
                     os.path.join(ROM_DIR, ("build" if args.mixer == "vanilla" else "build-hq") + rate_suffix, "accuracy_test.gba")):
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
    print(f"ROM: {args.rom} (mixer {args.mixer}; engine asked for {args.engine_mixer}; PCM rate {args.pcm_rate} Hz)\nmgba: {ver}\nreferences: {ref_dir}\n")
    hdr = f"{'test':18s} {'lag ms':>7s} {'gain dB':>8s} | {'null':>7s} {'worst1s':>7s} | {'gm null':>7s} {'gm wrst':>7s} | {'ref':>6s} {'test':>6s} | {'drift':>7s} | gate {args.gate:.0f}"
    print(hdr)
    print("-" * len(hdr))
    for t in tests:
        ref = render_reference(args, t, ref_dir)
        if args.update_refs and t["group"] == "micro" and args.mgba_render:
            commit_ref(ref, t["name"])
        out = render_poryaaaa(args, t, overlay, out_dir)
        if out is None:
            continue
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
              "label": args.label, "input": args.input, "mixer": args.mixer, "engine_mixer": args.engine_mixer,
              "pcm_rate": args.pcm_rate, "results": results}
    if args.input == "midi":
        report["midi_quantize"] = bool(args.midi_quantize)   # the midi baseline families
        report["gba_start_delay"] = bool(args.gba_start_delay)
        report["gba_tempo_grid"] = bool(args.gba_tempo_grid)
    path = args.json or os.path.join(args.build_dir, ("report" if args.mixer == "vanilla" else "report-hq") + rate_suffix + ".json")
    with open(path, "w") as f:
        json.dump(report, f, indent=1)
    print(f"report: {path}")
    return 0 if n_pass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
