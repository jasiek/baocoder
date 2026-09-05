#!/usr/bin/env python3
"""Sweep the voiced synthesiser's helpers under the emulator, one call per case.

Vocoder_SynthesizeVoiced 0x0001DE10 is 2378 bytes over four functions, and
tests/fixtures/dm32_arc4_1.fwvoiced only answers the whole question.  These
sweep the helpers so each can be transcribed and settled on its own, the same
economy gen_basop_jobs.py buys for the two block-float leaves.

Vocoder_ComputeHarmonicGains 0x0001D71C is the innermost and the easiest to
sweep: it reads nothing but its seven arguments and writes two arrays.

    Vocoder_ComputeHarmonicGains(pOutGain, pOutIndex, nPhase, nPrevPitch,
                                 nPitchDelta, nCount, nHarmonicCount)
                                    r0        r1       r2      r3(short)
                                                        + three on the stack

The cases carry the real corpus's shape - a sample count of 0x50, pitches
either side of the 0x1079 the pitchless branch parks on - and then the edges the
degenerate branches need: a zero phase error, which makes the first
normalisation land on the 0x1f exponent path, a zero previous pitch and a zero
count, which are the only ways to reach 0x0001D972 and 0x0001D982.

    python3 tools/fw_oracle/gen_vhelper_jobs.py /tmp/vh.job
    EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/vh.job /tmp/vh.out
    python3 tools/fw_oracle/gen_vhelper_jobs.py --export /tmp/vh.out \
            tests/fixtures/voiced_gains.fw

SPDX-License-Identifier: ISC
"""
import os, struct, sys

REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))
import emu

GAINS = 0x0001D71C
SPEC  = 0x0001D4C8
INTERP = 0x0001D9F0
OUT_G = 0x00051000        # 0x38 ushort
OUT_I = 0x00051100        # 0x38 ushort
NMAX  = 0x38

# Vocoder_SynthesizeHarmonicSpectrum's five buffers.  The destination is 0x100
# shorts seen as 128 complex bins plus one more for the wrap sample the stock
# code writes at pDest[0x100], so it is given 0x102 shorts of room and the short
# after it is checked for having stayed 0xEEEE.
S_DEST  = 0x00051200      # 0x102 shorts
S_EXP   = 0x00051500      # one short, written through param_2
S_PHASE = 0x00051600      # NMAX ushort
S_VOICE = 0x00051700      # NMAX short
S_AMP   = 0x00051800      # NMAX ushort
NDEST   = 0x102

# Vocoder_InterpolateSpectralEnvelope's two buffers.  The accumulator is 0xA8
# ints - the stock code clamps its last write to 0xA6 and writes one past it -
# and is given four more so an overrun is visible rather than silent.
I_ENV   = 0x00051A00      # NENV ints
I_BLOCK = 0x00051D00      # 0x101 shorts, the block the spectrum builder wrote
NENV    = 0xA8 + 4
NBLOCK  = 0x101


def _cases():
    """(phase, prev_pitch, pitch_delta, n, L)."""
    out = []
    # the corpus's own shape: n is always 0x50, pitches around the 0x1079 the
    # pitchless branch parks on and the 0x4027 the b0 >= 120 path writes
    for pitch in (0x1079, 0x4027, 0x0800, 0x2000, 0x7FFF, 1, -1, -0x4000):
        for delta in (0, 1, -1, 0x100, -0x100, 0x4000, -0x4000, 0x7FFF):
            for phase in (0, 0x10000, 0x8000, -0x10000, 0x7FFF0000, -0x7FFF0000):
                out.append((phase, pitch, delta, 0x50, 16))
    # the degenerate branches, which no real frame is likely to reach
    for L in (1, 2, 8, 56):
        out.append((0x10000, 0, 0, 0x50, L))        # 2*p^2 == 0
        out.append((0x10000, 0x1079, 0, 0, L))      # 2*n^2 == 0
        out.append((0, 0, 0, 0, L))
        out.append((0x10000, 0x1079, 0, 0x50, L))   # a plain one at each L
    # a deterministic sweep over the whole argument space
    x = 987654321
    for _ in range(1200):
        v = []
        for _ in range(4):
            x = (1103515245 * x + 12345) & 0x7FFFFFFF
            v.append(x)
        out.append((v[0] - 0x40000000, ((v[1] >> 7) & 0xFFFF) - 0x8000,
                    ((v[2] >> 7) & 0xFFFF) - 0x8000, 0x50,
                    1 + ((v[3] >> 7) % NMAX)))
    return out


def _spec_cases():
    """(step, exp_bias, end, start, mark, phase[], voiced[], amp[]).

    The shape of a real frame first - a run of voiced harmonics with amplitudes
    that fall away, phases walking with the pitch - and then the cases the stock
    code branches on: no voiced harmonic at all, which is the -0x20 exponent
    path; start >= end, which returns before it looks at anything; a single
    harmonic; and amplitudes large enough that the block-float shift goes
    negative.

    The two saturating phases are not reachable by choosing arguments here -
    they need an exact 0x80000000 out of the phase accumulator - so they are
    swept for rather than constructed: the deterministic sweep runs the step and
    the phases over their whole range, and the export reports how many of the
    1600 cases wrote no bin, which is the closest observable.
    """
    out = []
    x = 55555

    def rnd(mod):
        nonlocal x
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        return (x >> 7) % mod

    def frame(L, mark, step, bias, start=0, all_voiced=True):
        ph = [rnd(0x10000) for _ in range(NMAX)]
        vo = [(mark if (all_voiced or rnd(3)) else 0) for _ in range(NMAX)]
        am = [max(0, 0x4000 - i * 0x100 + rnd(0x400)) for i in range(NMAX)]
        return (step, bias, L, start, mark, ph, vo, am)

    for L in (1, 2, 8, 16, 32, 56):
        for step in (0, 1, -1, 0x100, -0x100, 0x1079, 0x4027, 0x7FFF, -0x8000):
            out.append(frame(L, 2, step, 0))
    for bias in (0, 1, -1, 16, -16, 0x100):
        out.append(frame(16, 2, 0x1079, bias))
    # the branches: nothing voiced, an empty range, one harmonic, a mark that
    # is not 2 - the caller passes 2, but the flag array holds other values
    z = frame(16, 2, 0x1079, 0)
    out.append((z[0], z[1], z[2], z[3], 2, z[5], [0] * NMAX, z[7]))
    out.append((z[0], z[1], 4, 4, 2, z[5], z[6], z[7]))
    out.append((z[0], z[1], 4, 9, 2, z[5], z[6], z[7]))
    out.append((z[0], z[1], 1, 0, 2, z[5], z[6], z[7]))
    out.append((z[0], z[1], 16, 0, 1, z[5], [1] * NMAX, z[7]))
    # amplitudes at the top of the range, where the shift goes negative
    out.append((0x1079, 0, 16, 0, 2, z[5], z[6], [0x7FFF] * NMAX))
    out.append((0x1079, 0, 16, 0, 2, z[5], z[6], [1] * NMAX))
    # 350, not the 1400 the gain sweep uses: every record here carries the whole
    # 0x102-short destination buffer, so the fixture is 7 KB a case rather than
    # 250 bytes.  The structured cases above are what reach the branches; the
    # random ones are there to catch a phase or an amplitude the structure
    # missed, and 350 of those is 900 KB rather than 3.5 MB.
    for _ in range(350):
        L = 1 + rnd(NMAX)
        start = rnd(L)
        out.append((rnd(0x10000) - 0x8000, rnd(41) - 20, L, start, 2,
                    [rnd(0x10000) for _ in range(NMAX)],
                    [(2 if rnd(4) else 0) for _ in range(NMAX)],
                    [rnd(0x8000) for _ in range(NMAX)]))
    return out


def _interp_cases():
    """(mant, exp, pitch, block_exp, env[], block[]).

    `mant`/`exp` place the harmonic in the accumulator as a block float, and
    the interesting values are the ones near the edges of it: a start before
    zero, which shortens the rising taper by a 16-bit truncation rather than
    clipping it, a start below -0x10, which returns having written nothing, and
    an end past 0xA6, which is clamped.  The envelope is poked with a recognisable
    ramp rather than zeros, because this function ACCUMULATES - a transcription
    that assigned instead of adding would pass against a zeroed buffer.

    Cases whose start lands OUTSIDE the accumulator are filtered out, and the
    reason is not squeamishness: the stock code does not range-check its
    destination, so it happily writes 0x10 taper samples at pDest + start for any
    start its caller hands it.  Under the emulator that lands in scratch and the
    peek never sees it; in a C transcription it is a wild write.  The caller
    cannot produce such a start - the block float comes from bounded gains - so
    the sweep does not either, and the function is documented as trusting its
    caller the way the stock one does.
    """
    def start_index(mant, exp):
        """The same arithmetic the function opens with, to filter on."""
        sh = ((exp - 0xf) + 0x8000) % 0x10000 - 0x8000
        v = (mant << 16) & 0xFFFFFFFF
        if v >= 0x80000000:
            v -= 0x100000000
        if sh < 0:
            n = min(-sh, 63)
            v = v >> n if n < 32 else (-1 if v < 0 else 0)
        else:
            n = min(sh, 63)
            v = ((v << n) & 0xFFFFFFFF) if n < 32 else 0
        first = ((v & 0xFFFFFFFF) + 0xffff) & 0xFFFFFFFF
        i = (first >> 16) & 0xFFFF
        return i - 0x10000 if i >= 0x8000 else i

    def in_range(mant, exp):
        # The rising taper is 0x10 long and runs from the start unconditionally
        # - only the END is clamped to 0xA6 - so a start at 0xA6 writes to 0xB6,
        # past the 0xA8-int accumulator.  The caller never places one there; the
        # sweep must not either, or the fixture records the firmware scribbling.
        i = start_index(mant, exp)
        return -0x20 <= i <= 0x90
    out = []
    x = 13579

    def rnd(mod):
        nonlocal x
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        return (x >> 7) % mod

    env0  = [((i * 37) & 0xFFFF) - 0x8000 for i in range(NENV)]
    block = [(i * 251) & 0xFFFF for i in range(NBLOCK)]

    for pitch in (0x1079, 0x4027, 0x0800, 0x2000, 0x7FFF, 1):
        for exp in (0, 4, 8, 0xf, 0x10, 0x14, -4):
            for mant in (0, 1, 0x100, 0x4000, 0x7FFF, -1, -0x100, -0x4000,
                         0x20, 0x50, 0xa6, -0x10, -0x11, -0x1f):
                if in_range(mant, exp):
                    out.append((mant, exp, pitch, 0x10, env0, block))
    for bexp in (0, 0x10, 0x18, 8, -8):
        out.append((0x50, 0xf, 0x1079, bexp, env0, block))
    tries = 0
    while len(out) < 900 and tries < 20000:
        tries += 1
        mant, exp = rnd(0x10000) - 0x8000, rnd(41) - 20
        if not in_range(mant, exp):
            continue
        out.append((mant, exp, rnd(0x10000), rnd(41) - 12, env0,
                    [rnd(0x10000) for _ in range(NBLOCK)]))
    return out


def gen(jobfile):
    cases = _cases()
    j = emu.Job()
    for i, (phase, pitch, delta, n, L) in enumerate(cases):
        j.poke(OUT_G, b"\xEE" * (NMAX * 2))
        j.poke(OUT_I, b"\xEE" * (NMAX * 2))
        j.call(GAINS, OUT_G, OUT_I, phase & 0xFFFFFFFF, pitch & 0xFFFFFFFF,
               delta & 0xFFFFFFFF, n & 0xFFFFFFFF, L & 0xFFFFFFFF)
        j.peek("g%d" % i, OUT_G, NMAX * 2)
        j.peek("x%d" % i, OUT_I, NMAX * 2)
    for i, (step, bias, end, start, mark, ph, vo, am) in enumerate(_spec_cases()):
        j.poke(S_DEST,  b"\xEE" * (NDEST * 2))
        j.poke(S_EXP,   b"\xEE\xEE")
        j.poke(S_PHASE, struct.pack("<%dH" % NMAX, *ph))
        j.poke(S_VOICE, struct.pack("<%dh" % NMAX, *vo))
        j.poke(S_AMP,   struct.pack("<%dH" % NMAX, *am))
        j.call(SPEC, S_DEST, S_EXP, step & 0xFFFFFFFF, S_PHASE, S_VOICE, S_AMP,
               bias & 0xFFFFFFFF, end & 0xFFFFFFFF, start & 0xFFFFFFFF,
               mark & 0xFFFFFFFF)
        j.getreg("r0")
        j.peek("sd%d" % i, S_DEST, NDEST * 2)
        j.peek("se%d" % i, S_EXP, 2)
    for i, (mant, exp, pitch, bexp, env, blk) in enumerate(_interp_cases()):
        j.poke(I_ENV,   struct.pack("<%di" % NENV, *env))
        j.poke(I_BLOCK, struct.pack("<%dH" % NBLOCK, *blk))
        j.call(INTERP, I_ENV, mant & 0xFFFFFFFF, exp & 0xFFFFFFFF,
               pitch & 0xFFFFFFFF, I_BLOCK, bexp & 0xFFFFFFFF)
        j.peek("ie%d" % i, I_ENV, NENV * 4)
    j.write(jobfile)
    print("wrote %s: %d gain, %d spectrum, %d interpolate"
          % (jobfile, len(cases), len(_spec_cases()), len(_interp_cases())))
    return len(cases)


def export(outfile, dest):
    """Write the cases that completed, and say how many did not.

    Vocoder_ComputeHarmonicGains is not a total function.  Its divide is
    Math_FloatDivExponent and the divisor is the denominator's mantissa, so an
    argument set where that normalises to zero makes `divs` trap - which is a
    fact about the radio, not about the sweep.  Sixteen of these 1600 do it, all
    of them deliberate edges: a zero sample count, a zero previous pitch with no
    pitch movement, and prev_pitch = -0x4000 with delta = -1, where the two terms
    under the root cancel.  They are dropped here rather than excluded from the
    case list, so the count of them stays visible: a transcription cannot be
    asked what the radio does on an input the radio faults on.
    """
    cases = _cases()
    res = emu.parse(outfile)
    assert not res["errors"], res["errors"][:2]
    ok = []
    for line in open(outfile):
        if line.startswith("CALL "):
            ok.append(True)
        elif line.startswith("FAULT "):
            ok.append(False)
    # the job runs the gain cases first, then the spectrum ones
    assert len(ok) == len(cases) + len(_spec_cases()) + len(_interp_cases()), (
        "%d outcomes for %d + %d + %d calls" % (len(ok), len(cases),
        len(_spec_cases()), len(_interp_cases())))
    rest = ok[len(cases):]
    assert all(rest), "%d later calls faulted" % rest.count(False)
    n = skipped = 0
    with open(dest, "w") as fh:
        fh.write("# Vocoder_ComputeHarmonicGains 0x0001D71C, called directly under the\n"
                 "# p-code emulator.  tools/fw_oracle/gen_vhelper_jobs.py.\n"
                 "# per record: phase prevPitch pitchDelta n L, then L gains and L\n"
                 "#   exponents - the two arrays it writes, both ushort.  The output\n"
                 "#   buffers are poked 0xEEEE first, so a short it did not write is\n"
                 "#   visible as 61166 rather than as a plausible zero.\n"
                 "# Cases whose denominator normalises to zero are absent: `divs` traps\n"
                 "# on the radio, so there is no answer to record.\n")
        for i, (phase, pitch, delta, cnt, L) in enumerate(cases):
            if not ok[i]:
                skipped += 1
                continue
            g = struct.unpack("<%dH" % NMAX, res["peek"]["g%d" % i][0])
            x = struct.unpack("<%dH" % NMAX, res["peek"]["x%d" % i][0])
            fh.write("%d %d %d %d %d %s %s\n" % (
                phase, pitch, delta, cnt, L,
                " ".join(map(str, g[:L])), " ".join(map(str, x[:L]))))
            n += 1
    print("%s: %d cases, %d excluded because the divide traps" % (dest, n, skipped))

    r0 = [v for nm, v in res["regs"] if nm == "r0"]
    sdest = dest.replace("gains", "spectrum")
    with open(sdest, "w") as fh:
        fh.write("# Vocoder_SynthesizeHarmonicSpectrum 0x0001D4C8, called directly\n"
                 "# under the p-code emulator.  tools/fw_oracle/gen_vhelper_jobs.py.\n"
                 "# per record: step expBias end start mark, %d phases, %d voicing\n"
                 "#   flags, %d amplitudes, then the bins written back - %d shorts,\n"
                 "#   which is 128 complex bins and the wrap sample at [0x100] - the\n"
                 "#   exponent written through param_2, and the return value.\n"
                 "# The destination is poked 0xEEEE first, so a short the firmware did\n"
                 "# not write is visible rather than passing as a zero.\n"
                 % (NMAX, NMAX, NMAX, NDEST))
        m = 0
        for i, (step, bias, end, start, mark, ph, vo, am) in enumerate(_spec_cases()):
            d = struct.unpack("<%dh" % NDEST, res["peek"]["sd%d" % i][0])
            e = struct.unpack("<h", res["peek"]["se%d" % i][0])[0]
            fh.write("%d %d %d %d %d %s %s %s %s %d %d\n" % (
                step, bias, end, start, mark,
                " ".join(map(str, ph)), " ".join(map(str, vo)),
                " ".join(map(str, am)), " ".join(map(str, d)), e,
                r0[i] - 0x10000 if r0[i] >= 0x8000 else r0[i]))
            m += 1
    print("%s: %d spectrum cases" % (sdest, m))

    idest = dest.replace("gains", "interp")
    with open(idest, "w") as fh:
        fh.write("# Vocoder_InterpolateSpectralEnvelope 0x0001D9F0, called directly\n"
                 "# under the p-code emulator.  tools/fw_oracle/gen_vhelper_jobs.py.\n"
                 "# per record: mant exp pitch blockExp, %d ints of accumulator in,\n"
                 "#   %d shorts of block, then %d ints of accumulator out.  The input\n"
                 "#   accumulator is a ramp, not zeros: this function ADDS, and a\n"
                 "#   transcription that assigned would pass against a zeroed buffer.\n"
                 % (NENV, NBLOCK, NENV))
        q = 0
        for i, (mant, exp, pitch, bexp, env, blk) in enumerate(_interp_cases()):
            o = struct.unpack("<%di" % NENV, res["peek"]["ie%d" % i][0])
            fh.write("%d %d %d %d %s %s %s\n" % (
                mant, exp, pitch, bexp, " ".join(map(str, env)),
                " ".join(map(str, blk)), " ".join(map(str, o))))
            q += 1
    print("%s: %d interpolate cases" % (idest, q))
    return n


if __name__ == "__main__":
    if sys.argv[1] == "--export":
        export(sys.argv[2], sys.argv[3])
    else:
        gen(sys.argv[1])
