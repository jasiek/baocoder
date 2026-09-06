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
MATCH    = 0x000277F8
NEXP     = 0x00051A00
ASRC     = 0x00051800
ADST     = 0x00051900
HSRC     = 0x00051600
HDST     = 0x00051700
PARAMS   = 0x00051200
FLAGS    = 0x00051400
BLK      = 0x00051000
EXPO     = 0x00051100
MAMP     = 0x00051B00
MEXP     = 0x00051C00
MREFM    = 0x00051C10
MREFE    = 0x00051C20


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


def _match_cases():
    """(56 amplitudes, blockExp, refMant, refExp, pitch, count).

    Vocoder_MatchExcitationEnergy 0x000277F8, the spectral amplitude
    enhancement.  Three axes decide it and each needs its own end of a range:

      the harmonic count, because the low L/8 is halved and skipped, so a count
      below eight has no enhanced band at all and a count of 1 has no low band;

      the pitch, because it is the phase step into the cosine table - l*w0
      lands on index l*pitch/1024, and the stock code does not mask that index,
      so the cases have to keep L*w0 under pi, which is where a real frame's
      pitch always sits and is half the table;

      and the amplitudes, because R1 = sum M^2 cos(l*w0) is what separates a
      flat envelope, where R1 approaches R0 and the weight collapses, from a
      decaying one.

    The lower bound on L*pitch is not tidiness.  The enhancement divides by
    w0*R0*(R0^2 - R1^2) and the stock code has no guard on it: when L*w0 is
    small enough that the cosine weighting is indistinguishable from unity,
    R0^2 - R1^2 underflows the 16 bits it is normalised into, the divisor
    reaches Math_SDivHalf as zero and `divs` traps.  A first sweep with pitches
    down to 64 faulted on 14 of 320 cases that way, and it is not only the
    pitch that gets there: one harmonic dominating the envelope makes
    R1 = R0*cos(k*w0) for that k alone, so R0^2 - R1^2 goes as sin^2(k*w0) and
    collapses the same way when the dominant harmonic sits low.  A real frame
    is nowhere near either - its harmonics fill the band, L*w0 is close to pi -
    so the floor here is L*w0 >= pi/8, which clears every case that faulted
    with room to spare.  The pitch also has to fit a halfword: the stock code
    reads it with `ld.h`, so 0x10000 arrives as a zero pitch and traps on the
    same divide.
    """
    out = []
    x = 77213

    def rnd(mod):
        nonlocal x
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        return (x >> 7) % mod

    def amps(f):
        return [f(i) for i in range(0x38)]

    LO, HI, PMAX = 0x8000, 0x40000, 0x8000   # L*w0 in [pi/8, pi]; pitch is a short
    shapes = [
        lambda i: 0,                                    # silent: R0 is zero
        lambda i: 0x2000,                               # flat
        lambda i: 0x7FFF,                               # full scale
        lambda i: max(1, 0x7FFF >> (i // 4)),           # decaying
        lambda i: (i * 0x200) & 0x7FFF,                 # rising
        lambda i: 0x4000 if (i & 1) else 0x100,         # alternating
        lambda i: -0x7FFF + i * 0x100,                  # signed
        lambda i: 0x7FFF if i == 3 else 0x40,           # one dominant harmonic
    ]
    def band(count):
        """The pitches that keep L*w0 in [pi/8, pi] and fit a halfword.  A
        count of 1 cannot reach pi/8 at all - one harmonic times the largest
        pitch a short holds is just under it - so its floor is that pitch."""
        return (min((LO + count - 1) // count, PMAX - 1),
                min(HI // count, PMAX - 1))

    for f in shapes:
        for count in (1, 2, 8, 9, 16, 0x2d, 0x37, 0x38):
            lo, hi = band(count)
            for num, den in ((1, 16), (1, 4), (1, 2), (15, 16)):
                pitch = min(max((HI * num // den + count - 1) // count, lo), hi)
                out.append((amps(f), 0, 0x4000, 0, pitch, count))
    # the block exponent and the caller's reference pair, which the energies
    # are carried against and which the floor at -0x11 is measured from
    for e in (0, 1, -1, 8, -8, 15, -20):
        for rm, re in ((0, 0), (1, -0x11), (0x7FFF, 5), (-0x8000, -30),
                       (0x4000, 30)):
            out.append((amps(lambda i: max(1, 0x6000 >> (i // 6))), e, rm, re,
                        0x2000, 0x20))
    while len(out) < 340:
        count = 1 + rnd(0x38)
        lo, hi = band(count)
        pitch = lo + rnd(hi - lo + 1)
        out.append(([rnd(0x10000) - 0x8000 for _ in range(0x38)],
                    rnd(41) - 20, rnd(0x10000) - 0x8000, rnd(41) - 20,
                    pitch, count))
    out += _match_saturating_cases()
    return out


def _match_saturating_cases(per_count=3):
    """Cases that drive R1's normalised mantissa to exactly 0x8000.

    The weighted energy R1 = sum M^2 cos(l*w0) is the one place in the function
    that saturates: a mantissa of 0x8000 is rewritten to 0x8001, because
    everything downstream reads it back with `mulsh`, where 0x8000 is -32768
    and would flip the sign of the correction.  A random sweep does not reach
    it - the window is 2^16 wide in a 2^30 range - so these are constructed,
    the way tests/test_voiced.c's octave-repair cases are.

    The construction: R1 normalises to 0x8000xxxx exactly when the accumulator
    lands just below a power of two, so start from every harmonic at full
    scale, which fixes R0, and tune ONE harmonic until the weighted sum lands
    in [-2^k, -2^k + 2^(k-15)).  It has to be one harmonic and not the whole
    envelope: making R1 as large as R0 also makes R0^2 - R1^2 collapse, which
    takes the branch that never reads R1 at all, and the case then proves
    nothing.  R0 stays two orders of magnitude above R1 here.
    """
    with open(emu.FIRMWARE, "rb") as fh:
        fh.seek(emu.SRAM_FILE_OFF + 0x1630)         # g_awSineTable512
        cos = list(struct.unpack("<512h", fh.read(1024)))

    def s16(v):
        v &= 0xFFFF
        return v - 0x10000 if v & 0x8000 else v

    def s32(v):
        v &= 0xFFFFFFFF
        return v - 0x100000000 if v & 0x80000000 else v

    def sq_of(a):
        return s16(s32(s32(a * a) * 2) >> 16)

    def term(sq, c):
        return s32(s32(sq * c) * 2)

    def top_half(acc):
        """the high half of the normalised mantissa, as the function makes it"""
        lo, hi = acc & 0xFFFFFFFF, (acc >> 32) & 0xFFFFFFFF
        if not (lo or hi):
            return 0
        mhi, mlo = hi, lo
        if hi & 0x80000000:
            mhi, mlo = ~hi & 0xFFFFFFFF, ~lo & 0xFFFFFFFF
        lzc = lambda x: 32 if x == 0 else 32 - x.bit_length()
        sh = lzc(mhi) - 33 if mhi else lzc(mlo) - 1
        if sh >= 0:
            mant = 0 if sh >= 32 else (lo << sh) & 0xFFFFFFFF
        elif -sh >= 32:
            mant = ((hi - (1 << 32) if hi >> 31 else hi) >> (-sh - 32)) & 0xFFFFFFFF
        else:
            mant = ((lo >> -sh) | (hi << 1 << (31 + sh))) & 0xFFFFFFFF
        return mant >> 16

    first = {}
    for a in range(0x8000):
        first.setdefault(sq_of(a), a)

    out = []
    for count in (0x38, 0x30, 0x20, 0x18, 0x10):
        got = 0
        for pitch in range(min(0x7FFF, 0x40000 // count), 0x8000 // count, -1):
            step = s32(s32(pitch * 0x200) * 2) >> 4
            cs = [cos[((k + 1) * step) >> 16] for k in range(count)]
            full = sum(term(sq_of(0x7FFF), c) for c in cs)
            for j in range(count):
                if cs[j] == 0 or got >= per_count:
                    continue
                rest = full - term(sq_of(0x7FFF), cs[j])
                for k in range(18, 31):
                    sq = (-(1 << k) - rest) // (2 * cs[j])
                    for cand in (sq - 1, sq, sq + 1):
                        if not 0 <= cand <= 0x7FFE or cand not in first:
                            continue
                        a = [0x7FFF] * count + [0] * (0x38 - count)
                        a[j] = first[cand]
                        acc = sum(term(sq_of(a[i]), cs[i]) for i in range(count))
                        if top_half(acc) == 0x8000:
                            out.append((a, 0, 0x4000, 0, pitch, count))
                            got += 1
                            break
                    if got >= per_count:
                        break
                if got >= per_count:
                    break
            if got >= per_count:
                break
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
    for i, (a, e, rm, re, pitch, count) in enumerate(_match_cases()):
        j.poke(MAMP,  struct.pack("<56h", *a))
        j.poke(MEXP,  struct.pack("<h", e))
        j.poke(MREFM, struct.pack("<h", rm))
        j.poke(MREFE, struct.pack("<h", re))
        j.call(MATCH, MAMP, MEXP, MREFM, MREFE, pitch & 0xFFFF,
               count & 0xFFFF)
        j.peek("ma%d" % i, MAMP, 56 * 2)
        j.peek("mx%d" % i, MEXP, 2)
        j.peek("mm%d" % i, MREFM, 2)
        j.peek("mr%d" % i, MREFE, 2)
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

    dest = os.path.join(destdir, "frame_matchenergy.fw")
    with open(dest, "w") as fh:
        fh.write("# Vocoder_MatchExcitationEnergy 0x000277F8, executed under the\n"
                 "# p-code emulator.  tools/fw_oracle/gen_frame_jobs.py.\n"
                 "# per record: count pitch blockExp refMant refExp, 56 amplitudes\n"
                 "# in, then the 56 written back, the block exponent, and the\n"
                 "# reference pair it left behind.\n")
        m = 0
        for i, (a, e, rm, re, pitch, count) in enumerate(_match_cases()):
            o  = struct.unpack("<56h", res["peek"]["ma%d" % i][0])
            eo = struct.unpack("<h", res["peek"]["mx%d" % i][0])[0]
            mo = struct.unpack("<h", res["peek"]["mm%d" % i][0])[0]
            ro = struct.unpack("<h", res["peek"]["mr%d" % i][0])[0]
            fh.write("%d %d %d %d %d %s %s %d %d %d\n" % (
                count, pitch, e, rm, re, " ".join(map(str, a)),
                " ".join(map(str, o)), eo, mo, ro))
            m += 1
    print("%s: %d cases" % (dest, m))
    assert k == len(r0), "%d register reads, %d consumed" % (len(r0), k)


if __name__ == "__main__":
    if sys.argv[1] == "--export":
        export(sys.argv[2], sys.argv[3])
    else:
        gen(sys.argv[1])
