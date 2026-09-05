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
OUT_G = 0x00051000        # 0x38 ushort
OUT_I = 0x00051100        # 0x38 ushort
NMAX  = 0x38


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
    j.write(jobfile)
    print("wrote %s: %d cases" % (jobfile, len(cases)))
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
    assert len(ok) == len(cases), "%d outcomes for %d cases" % (len(ok), len(cases))
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
    return n


if __name__ == "__main__":
    if sys.argv[1] == "--export":
        export(sys.argv[2], sys.argv[3])
    else:
        gen(sys.argv[1])
