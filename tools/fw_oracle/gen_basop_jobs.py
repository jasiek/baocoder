#!/usr/bin/env python3
"""Sweep Math_FloatAdd 0x00018DD8 and Math_FloatDivExponent 0x00018EF4 under the
emulator, one call per case, and write what the radio answers.

Both are leaves - no state, no memory beyond the one exponent they write - so
unlike the synthesisers they need no capture to learn their inputs.  They can
simply be swept, which is worth doing before anything that calls them is
written: `Vocoder_ComputeHarmonicGains 0x0001D71C` calls the pair four times per
harmonic, so a mantissa that is one bit wrong here is a divergence forty times
over in a single frame, hunted through 2378 bytes instead of thirty.

The cases are the edges first, then a deterministic sweep.  The edges are the
whole reason for doing this: every one of them is a branch in the stock code
that a plausible transcription gets wrong.

  exponent differences of 31, 32 and 33.  Math_FloatAdd aligns to
  max(expA, expB) + 1 and admits a difference of 31, so the smaller operand is
  shifted by exactly 32 - undefined in C, and on the machine a shift amount
  masked to six bits, which shifts everything out.  A transcription using C's
  own `>>` disagrees with the radio on 257 of the 3377 cases here, and on none
  of the ones a sweep that never reaches a difference of 31 would contain.

  0x80000000 as a numerator and -32768 as a divisor, which the divide saturates
  to 0x7FFFFFFF and 0x7FFF at 0x00018F92 and 0x00018F88.

  mantissas of 0 on either side, both, and results that normalise to zero.

  the sign combinations, because the sign is carried by an XOR of the operands
  and applied to the quotient, not to the operands.

A zero divisor is NOT swept: `divs` traps, which is what 337 faulted calls
looked like while the voiced oracle's arguments were still wrong.

    python3 tools/fw_oracle/gen_basop_jobs.py /tmp/basop.job
    EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/basop.job /tmp/basop.out
    python3 tools/fw_oracle/gen_basop_jobs.py --export /tmp/basop.out \
            tests/fixtures/basop_float.fw

SPDX-License-Identifier: ISC
"""
import os, struct, sys

REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))
import emu

FADD = 0x00018DD8
FDIV = 0x00018EF4
FSUB = 0x00018E5C
SQRT = 0x00019364
SQSC = 0x000193E0
EXP  = 0x00051000          # the short they write through their last argument


def _cases():
    """(mant_a, exp_a, mant_b, exp_b), mantissas as signed 16-bit."""
    out = []
    edges_m = [0, 1, -1, 2, -2, 0x7FFF, -0x8000, 0x4000, -0x4000, 0x0100, 0x5A82]
    edges_e = [0, 1, -1, 15, -15, 31, 32, 33, -31, -32, 100, -100]
    for a in edges_m:
        for b in edges_m:
            for ea, eb in ((0, 0), (0, 31), (31, 0), (0, 32), (32, 0),
                           (0, 33), (33, 0), (7, -7), (-7, 7)):
                out.append((a, ea, b, eb))
    for ea in edges_e:
        for eb in edges_e:
            out.append((0x5A82, ea, 0x3FFF, eb))
            out.append((-0x5A82, ea, 0x3FFF, eb))
    # a deterministic sweep, the same LCG the rest of this project uses for
    # reproducible pseudo-random vectors
    x = 12345
    for _ in range(2000):
        v = []
        for _ in range(4):
            x = (1103515245 * x + 12345) & 0x7FFFFFFF
            v.append(x)
        out.append((((v[0] >> 7) & 0xFFFF) - 0x8000, ((v[1] >> 7) % 81) - 40,
                    ((v[2] >> 7) & 0xFFFF) - 0x8000, ((v[3] >> 7) % 81) - 40))
    return out


def _sqrt_cases():
    """(mantissa, exponent) for Math_Sqrt 0x00019364.

    Swept for the same reason as the other two and with a sharper edge: this
    project already has an ambe_sqrt, transcribed and measured against libm at
    1.3e-3, and it is NOT this function.  The stock code returns its mantissa in
    the high half of the register, carries a rounding term into every stage, and
    rounds once more after the odd-exponent correction rather than before it.
    On this sweep the two agree on one case in 607, which is the sort of thing a
    tolerance-based test cannot see and a transcription downstream of it cannot
    survive.
    """
    out = [(0, 0), (1, 0), (0x7FFFFFFF, 0), (0x40000000, 1), (0x40000000, 2),
           (-1, 0), (-0x40000000, 0), (0x7FFFFFFF, 1), (2, -30), (0x10000, 15)]
    x = 24680
    for _ in range(600):
        v = []
        for _ in range(2):
            x = (1103515245 * x + 12345) & 0x7FFFFFFF
            v.append(x)
        # the exponent parity selects the 1/sqrt(2) correction, so both are swept
        out.append(((v[0] << 1) & 0xFFFFFFFF, (v[1] % 61) - 30))
    return out


def _sqrt_scaled_cases():
    """(mantissa, exponent, qFormat) for Math_SqrtScaled 0x000193E0.

    Math_Sqrt's polynomial with the answer shifted into a caller-chosen Q format
    rather than left as a pair, and a separate entry point rather than a wrapper:
    it rounds by 0x8000 into the high half at every step including the final
    scaling.  Vocoder_NormalizeSpectralBlock and Vocoder_SynthesizeFrame both
    call it, so it is swept before either is written.
    """
    out = [(0, 0, 0), (1, 0, 0), (0x7FFFFFFF, 0, 0), (0x40000000, 1, 15),
           (0x40000000, 2, 15), (-1, 0, 0), (-0x40000000, 0, 0),
           (0x10000, 15, 15), (0x10000, 16, 0), (0x10000, 15, 31),
           (0x10000, 15, -31)]
    x = 97531
    for _ in range(700):
        v = []
        for _ in range(3):
            x = (1103515245 * x + 12345) & 0x7FFFFFFF
            v.append(x)
        out.append(((v[0] << 1) & 0xFFFFFFFF, (v[1] % 61) - 30, (v[2] % 41) - 20))
    return out


def gen(jobfile):
    cases = _cases()
    j = emu.Job()
    for i, (ma, ea, mb, eb) in enumerate(cases):
        # Math_FloatAdd takes the 32-bit register as it finds it and masks to 16
        # bits itself; the divide sign-extends its divisor and reads its
        # numerator full width.  Both are handed exactly what the callers hand
        # them: a 16-bit mantissa in a 32-bit register.
        j.poke(EXP, b"\xEE\xEE")
        j.call(FADD, ma & 0xFFFFFFFF, ea & 0xFFFFFFFF, mb & 0xFFFFFFFF,
               eb & 0xFFFFFFFF, EXP)
        j.getreg("r0")
        j.peek("ae%d" % i, EXP, 2)
        j.poke(EXP, b"\xEE\xEE")
        j.call(FSUB, ma & 0xFFFFFFFF, ea & 0xFFFFFFFF, mb & 0xFFFFFFFF,
               eb & 0xFFFFFFFF, EXP)
        j.getreg("r0")
        j.peek("se%d" % i, EXP, 2)
        if (mb & 0xFFFF) != 0:                 # `divs` traps on a zero divisor
            j.poke(EXP, b"\xEE\xEE")
            j.call(FDIV, ma & 0xFFFFFFFF, ea & 0xFFFFFFFF, mb & 0xFFFFFFFF,
                   eb & 0xFFFFFFFF, EXP)
            j.getreg("r0")
            j.peek("de%d" % i, EXP, 2)
    for i, (m, e) in enumerate(_sqrt_cases()):
        j.poke(EXP, struct.pack("<h", e))
        j.call(SQRT, m & 0xFFFFFFFF, EXP)
        j.getreg("r0")
        j.peek("qe%d" % i, EXP, 2)
    for i, (m, e, q) in enumerate(_sqrt_scaled_cases()):
        j.call(SQSC, m & 0xFFFFFFFF, e & 0xFFFFFFFF, q & 0xFFFFFFFF)
        j.getreg("r0")
    j.write(jobfile)
    print("wrote %s: %d add/divide cases, %d square roots"
          % (jobfile, len(cases), len(_sqrt_cases())))
    return len(cases)


def export(outfile, dest):
    cases = _cases()
    res = emu.parse(outfile)
    assert not res["faults"], res["faults"][:2]
    assert not res["errors"], res["errors"][:2]
    r0 = [v for n, v in res["regs"] if n == "r0"]
    k, n, seen = 0, 0, []
    with open(dest, "w") as fh:
        fh.write("# Math_FloatAdd 0x00018DD8 and Math_FloatDivExponent 0x00018EF4,\n"
                 "# executed under the p-code emulator.  tools/fw_oracle/gen_basop_jobs.py.\n"
                 "# per record: mantA expA mantB expB  addMant addExp  divMant divExp\n"
                 "#             subMant subExp - Math_FloatSub 0x00018E5C, which\n"
                 "#             returns B - A rather than A - B\n"
                 "# A divMant/divExp of -1 means the case was not run: mantB is zero\n"
                 "# and the divide would trap.\n")
        for (ma, ea, mb, eb) in cases:
            am = r0[k] & 0xFFFF; k += 1
            ax = struct.unpack("<h", res["peek"]["ae%d" % n][0])[0]
            sm = r0[k] & 0xFFFF; k += 1
            sx = struct.unpack("<h", res["peek"]["se%d" % n][0])[0]
            if (mb & 0xFFFF) != 0:
                dm = r0[k] & 0xFFFF; k += 1
                dx = struct.unpack("<h", res["peek"]["de%d" % n][0])[0]
            else:
                dm = dx = -1
            fh.write("%d %d %d %d %d %d %d %d %d %d\n"
                     % (ma, ea, mb, eb, am, ax, dm, dx, sm, sx))
            n += 1
    with open(dest.replace("float", "sqrt"), "w") as fh:
        fh.write("# Math_Sqrt 0x00019364, executed under the p-code emulator.\n"
                 "# tools/fw_oracle/gen_basop_jobs.py.\n"
                 "# per record: mantissa exponentIn, then the returned mantissa -\n"
                 "#   which the stock code leaves in the HIGH half of the word - and\n"
                 "#   the exponent it wrote back.\n")
        for (m, e) in _sqrt_cases():
            sm = r0[k]; k += 1
            sx = struct.unpack("<h", res["peek"]["qe%d" % len(seen)][0])[0]
            seen.append(1)
            fh.write("%d %d %u %d\n" % (m - (1 << 32) if m >= (1 << 31) else m,
                                         e, sm, sx))
    with open(dest.replace("float", "sqrtscaled"), "w") as fh:
        fh.write("# Math_SqrtScaled 0x000193E0, executed under the p-code emulator.\n"
                 "# tools/fw_oracle/gen_basop_jobs.py.\n"
                 "# per record: mantissa exponent qFormat, then the value returned.\n")
        for (m, e, q) in _sqrt_scaled_cases():
            fh.write("%d %d %d %u\n"
                     % (m - (1 << 32) if m >= (1 << 31) else m, e, q, r0[k]))
            k += 1
    assert k == len(r0), "%d register reads, %d consumed" % (len(r0), k)
    print("%s: %d cases, %s: %d square roots"
          % (dest, n, dest.replace("float", "sqrt"), len(seen)))
    return n


if __name__ == "__main__":
    if sys.argv[1] == "--export":
        export(sys.argv[2], sys.argv[3])
    else:
        gen(sys.argv[1])
