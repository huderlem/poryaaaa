#!/usr/bin/env python3
"""Compare the DMA ring slots the HQ ROM wrote with the engine's, frame by frame.

    tools/hq_ring_compare.py <probe log> <engine HQ_DEBUG stderr>

The probe log is an HQ-ROM mgba_probe run (it dumps ` ringA=.. ringB=..`,
the slot just written, at C_downsampler_return); the engine side is
poryaaaa_render --hq-mixer run with HQ_DEBUG=1 in the environment, whose
stderr carries one `HQRING ringA=.. ringB=..` line per frame (and an
`HQDBG` line with the mix buffer words before the downsampler).  Prints
each differing frame with the first differing sample indices on each side
(A = right, B = left).  An identical ring with a differing render is
mgba's scheduler floor, not the mixer (docs/HQ-MIXER-ACCURACY.md, step 4).
"""
import re, sys

def main():
    probe, eng = sys.argv[1], sys.argv[2]
    rom = [(int(m.group(1)) - 1, m.group(2), m.group(3)) for m in
           re.finditer(r' f(\d+) .*C_downsampler_return.* ringA=([0-9A-F]+) ringB=([0-9A-F]+)', open(probe).read())]
    engl = [(m.group(1), m.group(2)) for m in
            re.finditer(r'HQRING ringA=([0-9A-F]+) ringB=([0-9A-F]+)', open(eng).read())]
    bad = 0
    for (f, a, b) in rom:          # probe fN = engine frame N - 1; the engine's first line is frame 2
        i = f - 2
        if i < 0 or i >= len(engl):
            continue
        ea, eb = engl[i]
        if a != ea or b != eb:
            da = [k // 2 for k in range(0, len(a), 2) if a[k:k + 2] != ea[k:k + 2]]
            db = [k // 2 for k in range(0, len(b), 2) if b[k:k + 2] != eb[k:k + 2]]
            print('frame %d: A differs at %d samples %s, B at %d %s' % (f, len(da), da[:8], len(db), db[:8]))
            for k in da[:3]:
                print('   A sample %d: rom %s engine %s' % (k, a[2 * k:2 * k + 2], ea[2 * k:2 * k + 2]))
            bad += 1
    print('rom frames %d, engine frames %d, differing %d' % (len(rom), len(engl), bad))

if __name__ == '__main__':
    main()
