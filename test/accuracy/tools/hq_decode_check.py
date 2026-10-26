#!/usr/bin/env python3
"""Reproduce the HQ mixer's decoded DPCM block buffer from the ROM (docs/HQ-MIXER-ACCURACY.md, step 2(e)).

  hq_decode_check.py <hq rom.gba> <hq syms.nm> <probe log with buf= dumps> <sample symbol> <frame> <first block> <nblocks> [clear_top]

Decodes `nblocks` blocks from `first block` (may be negative: the block address
wraps and the bytes come from mgba's open bus, `cpu->prefetch[1]` = the word two
ahead of the loading instruction, byte-selected by address & 3) exactly as
F_decode_compressed does (the delta table indexed by the whole second byte), lays
them out as the reversed store does (each block reversed, later blocks lower) with
`clear_top` zero bytes above, and compares with the raw log's buffer dump of that
mgba frame (fN).  Example (the reversed Bulbasaur cry's end frame):

  hq_decode_check.py build/accuracy/rom-hq/accuracy_test.gba build/probe-hq/syms.nm \\
      build/probe-hq/cry_bulbasaur_reverse.txt Cry_Bulbasaur 50 -2 2 128
"""
import re, sys

rom_path, syms_path, log_path, sample, frame, first, nblocks = sys.argv[1:8]
clear_top = int(sys.argv[8]) if len(sys.argv) > 8 else 0
frame, first, nblocks = int(frame), int(first), int(nblocks)
rom = open(rom_path, "rb").read()
syms = {}
for line in open(syms_path):
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)
F_ROM, T_ROM = syms["F_decode_compressed"], syms["delta_lookup_table"]
F_RAM = syms["SoundMainRAM_Buffer"] + (F_ROM - (syms["SoundMainRAM"] & ~1))
data = syms[sample] + 16                                   # WaveData header is 16 bytes


def romb(a):
    return rom[a - 0x08000000]


def word_at_ram(a):                                        # the IWRAM copy's word = the ROM's (unmodified words)
    o = a - F_RAM + F_ROM - 0x08000000
    return int.from_bytes(rom[o:o + 4], "little")


table = [romb(T_ROM + i) for i in range(256)]


def rd(addr, pc):
    if 0x08000000 <= addr < 0x0E000000:
        return romb(addr)
    return (word_at_ram(pc + 8) >> (8 * (addr & 3))) & 0xFF   # GBALoadBad: prefetch[1] for ARM code


def decode(addr):
    out = []
    r9 = addr
    r2 = rd(r9, F_RAM + 8); r9 += 1
    r3 = rd(r9, F_RAM + 12); r9 += 1
    lr, firstpair = 64, True
    while True:
        if not firstpair:
            r3 = rd(r9, F_RAM + 20); r9 += 1
            r2 = (table[r3 >> 4] + r1) & 0xFF
            r3 &= 0xF
        r1 = (table[r3] + r2) & 0xFF
        out += [r2, r1]
        firstpair = False
        lr -= 2
        if lr <= 0:
            break
    return out


blocks = []
for k in range(first, first + nblocks):
    # r9 = (negative sample offset) lsr 6, then mla by 0x21: for block k < 0 that is 0x21 * ((2^32 + 64k) >> 6) + data mod 2^32
    if k < 0:
        addr = (0x21 * (((1 << 32) + 64 * k) >> 6) + data) & 0xFFFFFFFF
    else:
        addr = data + 0x21 * k
    print(f"block {k}: address {addr:08X}")
    blocks.append(decode(addr))
model = bytearray()
for b in blocks:
    model[0:0] = bytes(reversed(b))
model += bytes(clear_top)
dump = None
for line in open(log_path):
    m = re.search(rf" f{frame} v\s*\d+ B [0-9A-F]+ C_select_highspeed_codepath_vla_r3 .* buf=([0-9A-F]+)", line)
    if m:
        dump = bytes.fromhex(m.group(1))
if dump is None:
    raise SystemExit(f"no buffer dump for frame f{frame} in {log_path}")
print(f"dump {len(dump)} bytes, model {len(model)} bytes: {'MATCH' if dump == bytes(model) else 'DIFFER'}")
if dump != bytes(model):
    print(" dump ", dump[:32].hex()); print(" model", bytes(model)[:32].hex())
