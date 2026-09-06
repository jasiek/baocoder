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


def gen(jobfile):
    j = emu.Job()
    for v, n in _popcount_cases():
        j.call(POPCOUNT, v & 0xFFFFFFFF, n)
        j.getreg("r0")
    j.write(jobfile)
    print("wrote %s: %d popcount cases" % (jobfile, len(_popcount_cases())))


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
    assert k == len(r0), "%d register reads, %d consumed" % (len(r0), k)


if __name__ == "__main__":
    if sys.argv[1] == "--export":
        export(sys.argv[2], sys.argv[3])
    else:
        gen(sys.argv[1])
