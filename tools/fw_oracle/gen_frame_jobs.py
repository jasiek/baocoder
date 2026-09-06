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


def gen(jobfile):
    j = emu.Job()
    for v, n in _popcount_cases():
        j.call(POPCOUNT, v & 0xFFFFFFFF, n)
        j.getreg("r0")
    for st, tg, w in _smooth_cases():
        j.call(SMOOTH, st & 0xFFFFFFFF, tg & 0xFFFFFFFF, w & 0xFFFFFFFF)
        j.getreg("r0")
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
    assert k == len(r0), "%d register reads, %d consumed" % (len(r0), k)


if __name__ == "__main__":
    if sys.argv[1] == "--export":
        export(sys.argv[2], sys.argv[3])
    else:
        gen(sys.argv[1])
