#!/usr/bin/env python3
"""Generate the accuracy micro-test MIDIs and their micro.cfg.

Each micro-test isolates one engine feature. The same .mid is compiled into the
test ROM by mid2agb (options from micro.cfg, same format as pokeemerald's
midi.cfg) and rendered directly by poryaaaa_render, so both sides play from a
single source. 24 PPQN at 120 BPM: one beat is 24 ticks and 0.5 s. Every test
finishes sounding before 3 s (the harness renders MICRO_SECONDS = 3.0).

Usage: gen_micro_midis.py <output dir>   (default: ../rom/micro)
"""
import os
import sys

import mido

BEAT = 24
VG = "-G_accuracy_micro"
BASE_OPTS = f"-E {VG} -V127"

# Voice indices in accuracy_micro.inc
V_SQ1, V_SQ1_ENV, V_SQ1_SWEEP, V_SQ1_ALT = 0, 1, 2, 3
V_SQ2, V_SQ2_ALT, V_WAVE, V_WAVE_ALT = 4, 5, 6, 7
V_NOISE, V_NOISE7 = 8, 9
V_PCM, V_PCM_ADSR, V_PCM_DRUM, V_PCM_CMP, V_PCM_BASS = 10, 11, 12, 13, 14
V_PCM_FIX_LOOP, V_PCM_REV = 15, 16
V_SYNTH_PULSE, V_SYNTH_PULSE_LFO, V_SYNTH_SAW, V_SYNTH_TRI = 17, 18, 19, 20

CC_MOD, CC_VOL, CC_PAN = 1, 7, 10
CC_BENDR, CC_LFOS, CC_MODT, CC_TUNE, CC_LFODL = 20, 21, 22, 24, 26


class Track:
    def __init__(self, voice, channel):
        self.channel = channel
        self.events = [(0, "program_change", {"program": voice})]
        self.cc(0, CC_VOL, 127)
        self.cc(0, CC_PAN, 64)

    def cc(self, t, control, value):
        self.events.append((t, "control_change", {"control": control, "value": value}))

    def note(self, t, key, dur, vel=127):
        self.events.append((t, "note_on", {"note": key, "velocity": vel}))
        self.events.append((t + dur, "note_off", {"note": key, "velocity": 0}))

    def bend(self, t, value):  # value: -8192..8191
        self.events.append((t, "pitchwheel", {"pitch": value}))

    def to_mido(self):
        tr = mido.MidiTrack()
        order = {"program_change": 0, "control_change": 1, "pitchwheel": 2, "note_off": 3, "note_on": 4}
        evs = sorted(enumerate(self.events), key=lambda e: (e[1][0], order[e[1][1]], e[0]))
        last = 0
        for _, (t, kind, kw) in evs:
            tr.append(mido.Message(kind, channel=self.channel, time=t - last, **kw))
            last = t
        tr.append(mido.MetaMessage("end_of_track", time=0))
        return tr


def write_song(path, tracks, bpm=120):
    mf = mido.MidiFile(type=1, ticks_per_beat=BEAT)
    cond = mido.MidiTrack()
    cond.append(mido.MetaMessage("time_signature", numerator=4, denominator=4, time=0))
    cond.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(bpm), time=0))
    cond.append(mido.MetaMessage("end_of_track", time=0))
    mf.tracks.append(cond)
    for tr in tracks:
        mf.tracks.append(tr.to_mido())
    mf.save(path)


def pattern(voice, keys=(60, 72, 48), ch=0):
    """Three notes of one voice: beat 0 (1 beat), beat 1.5 (1 beat), beat 3 (2 beats)."""
    t = Track(voice, ch)
    t.note(0, keys[0], BEAT)
    t.note(BEAT + BEAT // 2, keys[1], BEAT)
    t.note(3 * BEAT, keys[2], 2 * BEAT)
    return [t]


def build():
    tests = []  # (name, opts, tracks)

    def add(name, tracks, extra=""):
        tests.append((name, (BASE_OPTS + " " + extra).strip(), tracks))

    add("sq1", pattern(V_SQ1))
    add("sq1_env", pattern(V_SQ1_ENV))
    t = Track(V_SQ1_SWEEP, 0); t.note(0, 60, 2 * BEAT); t.note(3 * BEAT, 72, 2 * BEAT)
    add("sq1_sweep", [t])
    add("sq1_alt", pattern(V_SQ1_ALT))
    add("sq2", pattern(V_SQ2))
    add("sq2_alt", pattern(V_SQ2_ALT))
    add("wave", pattern(V_WAVE))
    add("wave_alt", pattern(V_WAVE_ALT))
    add("noise", pattern(V_NOISE, (60, 72, 36)))
    add("noise7", pattern(V_NOISE7, (60, 72, 36)))

    add("pcm", pattern(V_PCM, (60, 67, 55)))
    add("pcm_hi", pattern(V_PCM, (84, 91, 96)))
    t = Track(V_PCM_ADSR, 0); t.note(0, 60, 2 * BEAT); t.note(3 * BEAT, 60, 6); t.note(4 * BEAT, 72, BEAT)
    add("pcm_adsr", [t])
    t = Track(V_PCM_DRUM, 0)
    for i in range(4):
        t.note(i * BEAT // 2, 60, BEAT // 4)
    t.note(2 * BEAT, 64, BEAT // 2); t.note(3 * BEAT, 72, BEAT // 2); t.note(4 * BEAT, 48, BEAT)
    add("pcm_drum", [t])
    t = Track(V_PCM_CMP, 0); t.note(0, 60, 2 * BEAT); t.note(3 * BEAT, 67, 2 * BEAT)
    add("pcm_cmp", [t])
    add("pcm_reverb", pattern(V_PCM, (60, 67, 55)), "-R50")
    t = Track(V_PCM, 0)
    for k in (60, 64, 67, 72):
        t.note(0, k, 2 * BEAT)
    for k in (48, 52, 55, 60, 64, 67):
        t.note(3 * BEAT, k, 2 * BEAT)
    add("pcm_chord", [t])
    t = Track(V_PCM_BASS, 0); t.note(0, 40, BEAT); t.note(BEAT, 45, BEAT); t.note(2 * BEAT, 36, 2 * BEAT)
    add("pcm_bass", [t])

    t = Track(V_PCM, 0)
    t.cc(0, CC_PAN, 0); t.note(0, 60, BEAT)
    t.cc(BEAT + BEAT // 2, CC_PAN, 64); t.note(BEAT + BEAT // 2, 64, BEAT)
    t.cc(3 * BEAT, CC_PAN, 127); t.note(3 * BEAT, 67, BEAT)
    t.cc(4 * BEAT + BEAT // 2, CC_PAN, 32); t.note(4 * BEAT + BEAT // 2, 60, BEAT)
    add("pan", [t])

    t = Track(V_PCM, 0)
    t.cc(0, CC_VOL, 127); t.note(0, 60, BEAT)
    t.cc(BEAT + BEAT // 2, CC_VOL, 64); t.note(BEAT + BEAT // 2, 60, BEAT)
    t.cc(3 * BEAT, CC_VOL, 16); t.note(3 * BEAT, 60, BEAT)
    t.note(4 * BEAT + BEAT // 2, 60, BEAT)
    for i in range(1, 13):  # ramp 16 -> 127 inside the last note
        t.cc(4 * BEAT + BEAT // 2 + i * 2, CC_VOL, min(127, 16 + i * 10))
    add("vol", [t])

    t = Track(V_PCM, 0)
    t.cc(0, CC_VOL, 127); t.note(0, 60, BEAT, 127)
    t.note(BEAT + BEAT // 2, 60, BEAT, 64)
    t.note(3 * BEAT, 60, BEAT, 24)
    add("vel", [t])

    t = Track(V_PCM, 0)
    t.cc(0, CC_BENDR, 12); t.bend(0, 0); t.note(0, 60, 4 * BEAT)
    steps = 16
    for i in range(1, steps + 1):  # up one octave over 2 beats, then down two octaves
        t.bend(i * (2 * BEAT) // steps, min(8191, i * 8192 // steps))
    for i in range(1, steps + 1):
        t.bend(2 * BEAT + i * (2 * BEAT) // steps, 8191 - i * 16383 // steps)
    t.bend(4 * BEAT, 0); t.note(4 * BEAT + 6, 60, BEAT)
    add("bend", [t])

    for name, modt in (("lfo_vib", 0), ("lfo_trem", 1), ("lfo_pan", 2)):
        t = Track(V_PCM, 0)
        t.cc(0, CC_MODT, modt); t.cc(0, CC_LFOS, 40); t.cc(0, CC_MOD, 24)
        t.note(0, 60, 4 * BEAT)
        t.cc(4 * BEAT, CC_MOD, 0); t.note(4 * BEAT + 6, 67, BEAT)
        add(name, [t])
    t = Track(V_PCM, 0)
    t.cc(0, CC_MODT, 0); t.cc(0, CC_LFOS, 40); t.cc(0, CC_LFODL, 30); t.cc(0, CC_MOD, 24)
    t.note(0, 60, 4 * BEAT)
    add("lfo_delay", [t])
    t = Track(V_SQ1, 0)
    t.cc(0, CC_MODT, 0); t.cc(0, CC_LFOS, 40); t.cc(0, CC_MOD, 24)
    t.note(0, 60, 2 * BEAT)
    t.cc(3 * BEAT, CC_MODT, 1); t.note(3 * BEAT, 60, 2 * BEAT)
    add("lfo_psg", [t])

    t = Track(V_PCM, 0)
    t.cc(0, CC_TUNE, 64); t.note(0, 60, BEAT)
    t.cc(BEAT + BEAT // 2, CC_TUNE, 32); t.note(BEAT + BEAT // 2, 60, BEAT)
    t.cc(3 * BEAT, CC_TUNE, 96); t.note(3 * BEAT, 60, BEAT)
    t.cc(4 * BEAT + BEAT // 2, CC_TUNE, 0); t.note(4 * BEAT + BEAT // 2, 60, BEAT)
    add("tune", [t])
    t = Track(V_SQ1, 0)
    t.cc(0, CC_TUNE, 64); t.note(0, 60, BEAT)
    t.cc(BEAT + BEAT // 2, CC_TUNE, 32); t.note(BEAT + BEAT // 2, 60, BEAT)
    t.cc(3 * BEAT, CC_TUNE, 127); t.note(3 * BEAT, 60, BEAT)
    add("tune_psg", [t])

    a = Track(V_SQ1, 0); b = Track(V_SQ2, 1); c = Track(V_WAVE, 2); d = Track(V_NOISE, 3)
    a.note(0, 60, 2 * BEAT); b.note(0, 64, 2 * BEAT); c.note(0, 67, 2 * BEAT); d.note(0, 60, 2 * BEAT)
    a.note(3 * BEAT, 72, BEAT); b.note(3 * BEAT + 6, 76, BEAT); c.note(3 * BEAT + 12, 79, BEAT); d.note(3 * BEAT + 18, 72, BEAT)
    add("psg_mix", [a, b, c, d])

    a = Track(V_PCM, 0); b = Track(V_SQ1, 1); c = Track(V_PCM_BASS, 2)
    for k in (60, 64, 67):
        a.note(0, k, 4 * BEAT)
    b.note(0, 72, BEAT); b.note(BEAT, 74, BEAT); b.note(2 * BEAT, 76, 2 * BEAT)
    c.note(0, 36, 2 * BEAT); c.note(2 * BEAT, 43, 2 * BEAT)
    add("pcm_psg", [a, b, c])

    # HQ-mixer fast-path blocks at their boundaries (docs/HQ-MIXER-ACCURACY.md,
    # section 6).  The flute is 3344 Hz at key 60: ratio 1.0 at key 84, 2.0 at
    # 96.  pcm_ratio_1x: just above 1.0 (85), just below (83), just above 2.0
    # (97).  pcm_short_loop: the flute's 562-sample loop shorter than a
    # frame's advance (key 100: 564; 108: 896, two wraps a frame) and key 112
    # (1128 > 0x400 bytes: the fast path reads straight from ROM on the
    # first frame, then the unbuffered loop wraps twice a frame).
    add("pcm_ratio_1x", pattern(V_PCM, (85, 83, 97)))
    add("pcm_short_loop", pattern(V_PCM, (100, 108, 112)))
    # The FIX path with a loop (step 6): the flute at its own rate, the key
    # ignored; each note outlives the 1874 samples (0.14 s) and loops.
    add("pcm_fix_loop", pattern(V_PCM_FIX_LOOP))
    # Reverb at its maximum (step 7): the HQ seed scales by reverb >> 2 = 31.
    add("pcm_reverb_max", pattern(V_PCM, (60, 67, 55)), "-R127")
    # Step 8: the reversed uncompressed path (the pizzicato backwards, each
    # note running out), and the stack budget: the Bulbasaur cry (10512 Hz,
    # ratio 0.786 at key 60) at keys where a frame's blocks exceed the HQ
    # mixer's 1220-byte budget (keys 94: ratio 5.6, 1280+ bytes, and 96; key 72 plays) --
    # the HQ ROM freezes those channels silent, the vanilla ROM plays them.
    add("pcm_rev", pattern(V_PCM_REV))
    add("pcm_cmp_hi", pattern(V_PCM_CMP, (96, 72, 94)))
    # Step 9: the HQ mixer's synth paths (accuracy_micro_synth.inc: voices
    # 17-20, zero-size samples).  HQ-only rows: the vanilla ROM plays a
    # zero-size sample as a click, so accuracy.py skips synth_* unless
    # --mixer hq.  The release 165 runs the volume rounding through its
    # range; key 48 puts the saw's and triangle's jumps at a slow phase step.
    add("synth_pulse", pattern(V_SYNTH_PULSE))
    add("synth_pulse_lfo", pattern(V_SYNTH_PULSE_LFO))
    add("synth_saw", pattern(V_SYNTH_SAW))
    add("synth_tri", pattern(V_SYNTH_TRI))

    return tests


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "rom", "micro")
    os.makedirs(out, exist_ok=True)
    tests = build()
    with open(os.path.join(out, "micro.cfg"), "w") as cfg:
        for name, opts, tracks in tests:
            write_song(os.path.join(out, name + ".mid"), tracks)
            cfg.write(f"{name}.mid: {opts}\n")
    print(f"wrote {len(tests)} micro-tests to {out}")


if __name__ == "__main__":
    main()
