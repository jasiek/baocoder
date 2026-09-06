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
PITCHHIST = 0x0001A9E8
HILBERT  = 0x00029D1C
SHIFTCPY = 0x0001AB58
NORMARR  = 0x0001ADA0
SHIFTSAT = 0x0001AF5C
NEXP     = 0x00051A00
ASRC     = 0x00051800
ADST     = 0x00051900
HSRC     = 0x00051600
HDST     = 0x00051700
PARAMS   = 0x00051200
FLAGS    = 0x00051400
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


def _pitchhist_cases():
    """(68-short parameter block, 0x38 flags, candidate index).

    The block's [0x40] is a pointer to the flags, so the job pokes both and
    repoints it.  The interesting axes are the old index against the new - equal,
    adjacent, far apart - and a harmonic count either side of the [9, 0x38] clamp
    the function applies to it.
    """
    out = []
    x = 24242

    def rnd(mod):
        nonlocal x
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        return (x >> 7) % mod

    def blk(idx, count, p1):
        b = [0] * 68
        b[1] = p1
        b[2] = count
        b[3] = idx
        b[6] = 0x1079
        for i in range(0x38):
            b[8 + i] = ((i * 313) & 0x7FFF)
        return b

    for idx in (0, 1, 8, 0x20, 0x37):
        for cand in (0, 1, 8, 0x20, 0x37):
            for count in (0, 8, 9, 10, 0x37, 0x38):
                for p1 in (0, 1, 0x1000, 0x7FFF):
                    out.append((blk(idx, count, p1),
                                [1 if i == idx else 0 for i in range(0x38)],
                                cand))
    while len(out) < 700:
        idx, cand = rnd(0x38), rnd(0x38)
        out.append((blk(idx, rnd(0x40), rnd(0x10000)),
                    [rnd(2) for _ in range(0x38)], cand))
    return out


def _hilbert_cases():
    """(count, 0x38 source values).

    The filter reaches 19 places either side of every output, so what the cases
    have to vary is the count - short blocks lean almost entirely on the
    extensions, and only count > 0x2D reaches the reflection past 0x40 - and the
    last value, which is where the falling ramp starts and where saturation is
    reachable.
    """
    out = []
    x = 5150

    def rnd(mod):
        nonlocal x
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        return (x >> 7) % mod

    for count in (1, 2, 9, 0x2c, 0x2d, 0x2e, 0x37, 0x38):
        out.append((count, [0] * 0x38))
        out.append((count, [0x7FFF] * 0x38))
        out.append((count, [-0x8000] * 0x38))
        out.append((count, [(i * 601) % 0xFFFF - 0x8000 for i in range(0x38)]))
        out.append((count, [0x4000 - i * 0x200 for i in range(0x38)]))
        # a last value near the ends, where the ramp saturates almost at once
        v = [100] * 0x38
        v[count - 1] = -0x7FFF
        out.append((count, list(v)))
        v[count - 1] = 0x7FFF
        out.append((count, list(v)))
    for _ in range(300):
        out.append((1 + rnd(0x38),
                    [rnd(0x10000) - 0x8000 for _ in range(0x38)]))
    return out


def _shiftcopy_cases():
    """(count, shift, 32 source values).

    The shift is applied to a sign-extended short and truncated back, so a left
    shift discards the top rather than saturating - which is only visible on
    values whose high bits are set.  Zero is its own branch in the stock code.
    """
    out = []
    vals = [0, 1, -1, 0x7FFF, -0x8000, 0x4000, -0x4000, 0x0100,
            0x1234, -0x1234, 0x5A82, -0x5A82]
    src = [vals[i % len(vals)] for i in range(32)]
    for count in (0, 1, 2, 31, 32):
        for sh in (0, 1, 2, 15, 16, 31, -1, -2, -15, -16, -31):
            out.append((count, sh, list(src)))
    x = 1717
    for _ in range(200):
        v = []
        for _ in range(32):
            x = (1103515245 * x + 12345) & 0x7FFFFFFF
            v.append(((x >> 7) & 0xFFFF) - 0x8000)
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        out.append((1 + (x >> 7) % 32, ((x >> 3) % 41) - 20, v))
    return out


def _normarr_cases():
    """(count, exponent, 32 source values).

    The peak is found with a true negation, so -0x8000 stays negative and
    compares as SMALLER than everything - a block whose only extreme is -0x8000
    is the case that separates a true negation from a saturating one.  An
    all-zero block is the other one: it leaves the caller's exponent alone
    rather than driving it anywhere.
    """
    out = []
    def blk(f):
        return [f(i) for i in range(32)]
    for e in (0, 1, -1, 15, -15, 100):
        out.append((32, e, blk(lambda i: 0)))
        out.append((32, e, blk(lambda i: -0x8000)))
        out.append((32, e, [(-0x8000 if i == 5 else 3) for i in range(32)]))
        out.append((32, e, blk(lambda i: 1)))
        out.append((32, e, blk(lambda i: 0x7FFF)))
        out.append((32, e, blk(lambda i: (i * 997) % 0xFFFF - 0x8000)))
        out.append((0, e, blk(lambda i: 5)))
        out.append((1, e, blk(lambda i: 0x0100)))
    x = 5309
    for _ in range(200):
        v = []
        for _ in range(32):
            x = (1103515245 * x + 12345) & 0x7FFFFFFF
            v.append(((x >> 7) & 0xFFFF) - 0x8000)
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        out.append((1 + (x >> 7) % 32, ((x >> 3) % 41) - 20, v))
    return out


def _shiftsat_cases():
    """(count, dstExp, srcExp, 32 source values).

    Saturation is checked per element against that element's own headroom, so a
    block with one loud sample among quiet ones is the case that separates this
    from a whole-array renormalisation.  Zero short-circuits, which matters
    because the headroom of zero is 31 and would otherwise pass any shift.
    """
    out = []
    def blk(f):
        return [f(i) for i in range(32)]
    loud = [1] * 32
    loud[7] = 0x7FFF
    loud[19] = -0x8000
    for de, se in ((0, 0), (4, 0), (0, 4), (15, 0), (0, 15), (1, 0), (0, 1),
                   (31, 0), (0, 31), (-4, 4), (4, -4)):
        out.append((32, de, se, blk(lambda i: 0)))
        out.append((32, de, se, list(loud)))
        out.append((32, de, se, blk(lambda i: 0x7FFF)))
        out.append((32, de, se, blk(lambda i: -0x8000)))
        out.append((32, de, se, blk(lambda i: (i * 811) % 0xFFFF - 0x8000)))
        out.append((1, de, se, list(loud)))
    x = 24601
    for _ in range(200):
        v = []
        for _ in range(32):
            x = (1103515245 * x + 12345) & 0x7FFFFFFF
            v.append(((x >> 7) & 0xFFFF) - 0x8000)
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        out.append((1 + (x >> 7) % 32, ((x >> 3) % 41) - 20,
                    ((x >> 11) % 41) - 20, v))
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
    for i, (b, fl, cand) in enumerate(_pitchhist_cases()):
        blk = list(b)
        blk[0x40] = FLAGS & 0xFFFF
        blk[0x41] = (FLAGS >> 16) & 0xFFFF
        j.poke(PARAMS, struct.pack("<68h", *[(v + 0x10000) % 0x10000 - 0x10000
                                             if v > 0x7FFF else v for v in blk]))
        j.poke(FLAGS,  struct.pack("<56H", *fl))
        j.call(PITCHHIST, PARAMS, cand & 0xFFFFFFFF)
        j.peek("ph%d" % i, PARAMS, 68 * 2)
        j.peek("pf%d" % i, FLAGS, 56 * 2)
    for i, (count, vals) in enumerate(_hilbert_cases()):
        j.poke(HSRC, struct.pack("<56h", *vals))
        j.poke(HDST, b"\xEE" * (56 * 2))
        j.call(HILBERT, HDST, HSRC, count & 0xFFFFFFFF)
        j.peek("hb%d" % i, HDST, 56 * 2)
    for i, (count, sh, vals) in enumerate(_shiftcopy_cases()):
        j.poke(ASRC, struct.pack("<32h", *vals))
        j.poke(ADST, b"\xEE" * (32 * 2))
        j.call(SHIFTCPY, ADST, ASRC, count & 0xFFFFFFFF, sh & 0xFFFFFFFF)
        j.peek("sc%d" % i, ADST, 32 * 2)
    for i, (count, e, vals) in enumerate(_normarr_cases()):
        j.poke(ASRC, struct.pack("<32h", *vals))
        j.poke(ADST, b"\xEE" * (32 * 2))
        j.poke(NEXP, struct.pack("<h", e))
        j.call(NORMARR, ADST, ASRC, count & 0xFFFFFFFF, NEXP)
        j.peek("na%d" % i, ADST, 32 * 2)
        # NOT "ne2%d": that collides with the block normaliser's "ne%d" the
        # moment either index reaches 20, and emu.parse keys peeks by name.
        j.peek("nax%d" % i, NEXP, 2)
    for i, (count, de, se, vals) in enumerate(_shiftsat_cases()):
        j.poke(ASRC, struct.pack("<32h", *vals))
        j.poke(ADST, b"\xEE" * (32 * 2))
        j.call(SHIFTSAT, ADST, ASRC, count & 0xFFFFFFFF, de & 0xFFFFFFFF,
               se & 0xFFFFFFFF)
        j.peek("ss%d" % i, ADST, 32 * 2)
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

    dest = os.path.join(destdir, "frame_pitchhist.fw")
    with open(dest, "w") as fh:
        fh.write("# Vocoder_UpdatePitchHistoryBuffer 0x0001A9E8, executed under the\n"
                 "# p-code emulator.  tools/fw_oracle/gen_frame_jobs.py.\n"
                 "# per record: candidate, 68 shorts of block in, 56 flags in, then\n"
                 "# 68 shorts out and 56 flags out.  The block's [0x40] pointer is\n"
                 "# repointed at the poked flags before the call and is recorded as\n"
                 "# the emulator's address, not the radio's.\n")
        m = 0
        for i, (b, fl, cand) in enumerate(_pitchhist_cases()):
            o = struct.unpack("<68h", res["peek"]["ph%d" % i][0])
            f = struct.unpack("<56H", res["peek"]["pf%d" % i][0])
            fh.write("%d %s %s %s %s\n" % (
                cand, " ".join(map(str, b)), " ".join(map(str, fl)),
                " ".join(map(str, o)), " ".join(map(str, f))))
            m += 1
    print("%s: %d cases" % (dest, m))

    dest = os.path.join(destdir, "frame_hilbert.fw")
    with open(dest, "w") as fh:
        fh.write("# Dsp_HilbertTransform 0x00029D1C, executed under the p-code\n"
                 "# emulator.  tools/fw_oracle/gen_frame_jobs.py.\n"
                 "# per record: count, 56 source values, then the 56 it wrote.\n")
        m = 0
        for i, (count, vals) in enumerate(_hilbert_cases()):
            o = struct.unpack("<56h", res["peek"]["hb%d" % i][0])
            fh.write("%d %s %s\n" % (count, " ".join(map(str, vals)),
                                      " ".join(map(str, o))))
            m += 1
    print("%s: %d cases" % (dest, m))

    dest = os.path.join(destdir, "frame_shiftcopy.fw")
    with open(dest, "w") as fh:
        fh.write("# Math_ArrayShiftCopy 0x0001AB58, executed under the p-code\n"
                 "# emulator.  tools/fw_oracle/gen_frame_jobs.py.\n"
                 "# per record: count shift, 32 source values, then the 32 written.\n"
                 "# The destination is poked 0xEEEE first, so a slot the firmware\n"
                 "# left alone reads as -4370 rather than as a zero it chose.\n")
        m = 0
        for i, (count, sh, vals) in enumerate(_shiftcopy_cases()):
            o = struct.unpack("<32h", res["peek"]["sc%d" % i][0])
            fh.write("%d %d %s %s\n" % (count, sh, " ".join(map(str, vals)),
                                         " ".join(map(str, o))))
            m += 1
    print("%s: %d cases" % (dest, m))

    dest = os.path.join(destdir, "frame_normarray.fw")
    with open(dest, "w") as fh:
        fh.write("# Dsp_NormalizeArray 0x0001ADA0, executed under the p-code\n"
                 "# emulator.  tools/fw_oracle/gen_frame_jobs.py.\n"
                 "# per record: count exponentIn, 32 source values, then the 32\n"
                 "# written and the exponent left behind.\n")
        m = 0
        for i, (count, e, vals) in enumerate(_normarr_cases()):
            o = struct.unpack("<32h", res["peek"]["na%d" % i][0])
            eo = struct.unpack("<h", res["peek"]["nax%d" % i][0])[0]
            fh.write("%d %d %s %s %d\n" % (count, e, " ".join(map(str, vals)),
                                            " ".join(map(str, o)), eo))
            m += 1
    print("%s: %d cases" % (dest, m))

    dest = os.path.join(destdir, "frame_shiftsat.fw")
    with open(dest, "w") as fh:
        fh.write("# Math_ArrayShiftSaturate 0x0001AF5C, executed under the p-code\n"
                 "# emulator.  tools/fw_oracle/gen_frame_jobs.py.\n"
                 "# per record: count dstExp srcExp, 32 source values, then the 32\n"
                 "# written.\n")
        m = 0
        for i, (count, de, se, vals) in enumerate(_shiftsat_cases()):
            o = struct.unpack("<32h", res["peek"]["ss%d" % i][0])
            fh.write("%d %d %d %s %s\n" % (count, de, se,
                                            " ".join(map(str, vals)),
                                            " ".join(map(str, o))))
            m += 1
    print("%s: %d cases" % (dest, m))
    assert k == len(r0), "%d register reads, %d consumed" % (len(r0), k)


if __name__ == "__main__":
    if sys.argv[1] == "--export":
        export(sys.argv[2], sys.argv[3])
    else:
        gen(sys.argv[1])
