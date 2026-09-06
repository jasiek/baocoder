#!/usr/bin/env python3
"""Sweep the frame-synthesis layer's functions under the emulator.

Vocoder_SynthesizeFrame 0x00019DB8 is what stands between src/ambe_voiced.c
being exact and the decode path being able to use it, and it calls six things
this library did not have.  Each is swept here before the one above it is
written, the same order the voiced synthesiser was built in.

    python3 tools/fw_oracle/gen_frame_jobs.py /tmp/frame.job
    EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/frame.job /tmp/frame.out
    python3 tools/fw_oracle/gen_frame_jobs.py --export /tmp/frame.out tests/fixtures

SPDX-License-Identifier: ISC
"""
import os, struct, sys

REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))
import emu

POPCOUNT = 0x000189F4
SMOOTH   = 0x00022D7C
POW2SC   = 0x00019280
NORMBLK  = 0x00022C18
BLK      = 0x00051000
EXPO     = 0x00051100


def _popcount_cases():
    """(value, nbits).

    The bit count is an argument and the stock code implements it by shifting
    the unwanted bits off the top, so a case with n < 32 and rubbish in the high
    bits is the one that separates "count the low n" from "count them all".
    """
    out = []
    for n in (1, 2, 7, 8, 15, 16, 31, 32):
        for v in (0, 1, 0xFFFFFFFF, 0x55555555, 0xAAAAAAAA, 0x80000000,
                  0x7FFFFFFF, 0xFFFF0000, 0x0000FFFF, 0x12345678):
            out.append((v, n))
    x = 424242
    for _ in range(400):
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        v = (x << 1) & 0xFFFFFFFF
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        out.append((v, 1 + (x >> 7) % 32))
    return out


def _smooth_cases():
    """(state, target, vuv).

    The voicing word decides whether the smoother runs at all - more than seven
    of the sixteen crumbs voiced - so the cases straddle that: words with 0, 7,
    8 and 16 bands set, and random ones either side.  Without the boundary the
    sweep cannot tell the smoother from the identity.
    """
    out = []
    words = [0, 0x55555555, 0x5555, 0x15555555, 0x05555555, 0x01555555,
             0xFFFFFFFF, 0xAAAAAAAA, 0x40000001, 0x55550000]
    for w in words:
        for st in (0, 1, 0x1079, 0x4027, 0x7FFF, 0x8000, 0xFFFF):
            for tg in (0, 1, 0x1079, 0x7FFF, 0x8000, 0xFFFF):
                out.append((st, tg, w))
    x = 31337
    for _ in range(300):
        v = []
        for _ in range(3):
            x = (1103515245 * x + 12345) & 0x7FFFFFFF
            v.append(x)
        out.append(((v[0] >> 7) & 0xFFFF, (v[1] >> 7) & 0xFFFF,
                    ((v[2] << 1) & 0xFFFFFFFF)))
    return out


def _pow2_scaled_cases():
    """(mantissa, exponent, qFormat).

    The mantissa is shifted by exp-15 before the fraction is taken, and the
    integer part comes off the same word with a LOGICAL shift, so the cases have
    to include mantissas that shift up into the sign bit as well as ones that
    shift away to nothing.
    """
    out = [(0, 0, 0), (1, 0, 0), (0x7FFF, 15, 15), (-0x8000, 15, 15),
           (0x4000, 16, 0), (0x4000, 14, 0), (1, 31, 0), (1, -31, 0),
           (0x7FFF, 0, 0), (0x7FFF, 31, 31), (-1, 15, 0)]
    x = 606060
    for _ in range(700):
        v = []
        for _ in range(3):
            x = (1103515245 * x + 12345) & 0x7FFFFFFF
            v.append(x)
        out.append((((v[0] >> 7) & 0xFFFF) - 0x8000, (v[1] % 41) - 12,
                    (v[2] % 41) - 12))
    return out


def _normblk_cases():
    """(count, 0x38 coefficients).

    The clamp is +/-0x77FF rather than +/-0x7FFF, so values either side of it
    are what say whether a transcription used the right bound; the peak search
    is signed and seeded at INT32_MIN, so an all-negative block is what says it
    is not seeded at zero; and count < 1 is the fixed-exponent path.
    """
    out = []
    def blk(f):
        return [f(i) for i in range(0x38)]
    out.append((0, blk(lambda i: 0)))
    out.append((-1, blk(lambda i: 100)))
    out.append((1, blk(lambda i: 0x7FFF)))
    out.append((0x38, blk(lambda i: 0x77FF)))
    out.append((0x38, blk(lambda i: 0x7800)))       # just over the clamp
    out.append((0x38, blk(lambda i: -0x7800)))
    out.append((0x38, blk(lambda i: -0x7FFF)))      # entirely negative
    out.append((0x38, blk(lambda i: 0)))
    for L in (1, 2, 8, 16, 32, 0x37, 0x38):
        out.append((L, blk(lambda i: (i * 617) % 0xFFFF - 0x8000)))
        out.append((L, blk(lambda i: 0x4000 - i * 0x100)))
    x = 8642
    for _ in range(200):
        vals = []
        for _ in range(0x38):
            x = (1103515245 * x + 12345) & 0x7FFFFFFF
            vals.append(((x >> 7) & 0xFFFF) - 0x8000)
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        out.append((1 + (x >> 7) % 0x38, vals))
    return out


def gen(jobfile):
    j = emu.Job()
    for v, n in _popcount_cases():
        j.call(POPCOUNT, v & 0xFFFFFFFF, n)
        j.getreg("r0")
    for st, tg, w in _smooth_cases():
        j.call(SMOOTH, st & 0xFFFFFFFF, tg & 0xFFFFFFFF, w & 0xFFFFFFFF)
        j.getreg("r0")
    for m, e, q in _pow2_scaled_cases():
        j.call(POW2SC, m & 0xFFFFFFFF, e & 0xFFFFFFFF, q & 0xFFFFFFFF)
        j.getreg("r0")
    for i, (count, vals) in enumerate(_normblk_cases()):
        j.poke(BLK,  struct.pack("<56h", *vals))
        j.poke(EXPO, b"\xEE\xEE")
        j.call(NORMBLK, BLK, EXPO, count & 0xFFFFFFFF)
        j.peek("nb%d" % i, BLK, 56 * 2)
        j.peek("ne%d" % i, EXPO, 2)
    j.write(jobfile)
    print("wrote %s: %d popcount, %d smoother cases"
          % (jobfile, len(_popcount_cases()), len(_smooth_cases())))


def export(outfile, destdir):
    res = emu.parse(outfile)
    assert not res["faults"], res["faults"][:2]
    assert not res["errors"], res["errors"][:2]
    r0 = [v for n, v in res["regs"] if n == "r0"]
    k = 0
    dest = os.path.join(destdir, "frame_popcount.fw")
    with open(dest, "w") as fh:
        fh.write("# Math_PopCountBits 0x000189F4, executed under the p-code emulator.\n"
                 "# tools/fw_oracle/gen_frame_jobs.py.\n"
                 "# per record: value nbits, then the count returned.\n")
        for v, n in _popcount_cases():
            fh.write("%u %d %d\n" % (v, n, r0[k]))
            k += 1
    print("%s: %d cases" % (dest, k))

    dest = os.path.join(destdir, "frame_smooth.fw")
    with open(dest, "w") as fh:
        fh.write("# Vocoder_SmoothPitchState 0x00022D7C, executed under the p-code\n"
                 "# emulator.  tools/fw_oracle/gen_frame_jobs.py.\n"
                 "# per record: state target voicingWord, then the state returned.\n")
        m = 0
        for st, tg, w in _smooth_cases():
            fh.write("%u %u %u %u\n" % (st, tg, w, r0[k]))
            k += 1
            m += 1
    print("%s: %d cases" % (dest, m))

    dest = os.path.join(destdir, "frame_pow2scaled.fw")
    with open(dest, "w") as fh:
        fh.write("# Math_Pow2Scaled 0x00019280, executed under the p-code emulator.\n"
                 "# tools/fw_oracle/gen_frame_jobs.py.\n"
                 "# per record: mantissa exponent qFormat, then the value returned.\n")
        m = 0
        for mm, e, q in _pow2_scaled_cases():
            fh.write("%d %d %d %u\n" % (mm, e, q, r0[k]))
            k += 1
            m += 1
    print("%s: %d cases" % (dest, m))

    dest = os.path.join(destdir, "frame_normblock.fw")
    with open(dest, "w") as fh:
        fh.write("# Vocoder_NormalizeSpectralBlock 0x00022C18, executed under the\n"
                 "# p-code emulator.  tools/fw_oracle/gen_frame_jobs.py.\n"
                 "# per record: count, 56 coefficients in, 56 out, then the common\n"
                 "# exponent it wrote.\n")
        m = 0
        for i, (count, vals) in enumerate(_normblk_cases()):
            o = struct.unpack("<56h", res["peek"]["nb%d" % i][0])
            e = struct.unpack("<h", res["peek"]["ne%d" % i][0])[0]
            fh.write("%d %s %s %d\n" % (count, " ".join(map(str, vals)),
                                         " ".join(map(str, o)), e))
            m += 1
    print("%s: %d cases" % (dest, m))
    assert k == len(r0), "%d register reads, %d consumed" % (len(r0), k)


if __name__ == "__main__":
    if sys.argv[1] == "--export":
        export(sys.argv[2], sys.argv[3])
    else:
        gen(sys.argv[1])
