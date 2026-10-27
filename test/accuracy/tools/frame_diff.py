#!/usr/bin/env python3
"""Per-engine-frame error of a poryaaaa render against its mgba reference.

    tools/frame_diff.py <ref.wav> <test.wav> [max rows]

Both files at the same rate and channel count (the harness's
build/accuracy/refs/<key>/<name>.wav and build/accuracy/out/<name>.wav).
Prints how many 1/59.7275 s frames differ and, for the first rows, the
frame (1-based, output time: the ring slot mixed one engine frame earlier),
the error relative to the reference in that frame and the largest sample
difference in 1/32768 units.  A single frame at -50 dB with a max near 194
(one int8 DAC step after the output scaling) is mgba's scheduler floor:
check the frame against the probe's .events slot crossings
(docs/HQ-MIXER-ACCURACY.md, step 4).
"""
import sys, wave, struct, math

def load(p):
    w = wave.open(p); n = w.getnframes(); ch = w.getnchannels(); sw = w.getsampwidth()
    raw = w.readframes(n); rate = w.getframerate()
    if sw == 2:
        d = [x / 32768 for x in struct.unpack('<%dh' % (n * ch), raw)]
    elif sw == 4:
        d = struct.unpack('<%df' % (n * ch), raw)
    else:
        raise SystemExit('unsupported sample width %d' % sw)
    return rate, ch, d

def main():
    ra, ca, a = load(sys.argv[1]); rb, cb, b = load(sys.argv[2])
    if ra != rb or ca != cb:
        raise SystemExit('rate / channels differ: %d/%d vs %d/%d' % (ra, ca, rb, cb))
    fl = ra * 280896 / 16777216.0   # output samples per engine frame
    n = min(len(a), len(b)) // ca
    rows = []
    for f in range(int(n / fl)):
        s0 = int(f * fl) * ca; s1 = int((f + 1) * fl) * ca
        e = sum((a[i] - b[i]) ** 2 for i in range(s0, s1))
        if e > 0:
            r = sum(a[i] ** 2 for i in range(s0, s1))
            mx = max(abs(a[i] - b[i]) for i in range(s0, s1)) * 32768
            rows.append((f, 10 * math.log10(e / r) if r > 0 else float('inf'), mx))
    print('frames %d, differing %d, first %s' % (int(n / fl), len(rows), rows[0][0] + 1 if rows else '-'))
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    for f, db, mx in rows[:limit]:
        print('  frame %4d  err %6.1f dB  max %5.0f /32768' % (f + 1, db, mx))

if __name__ == '__main__':
    main()
