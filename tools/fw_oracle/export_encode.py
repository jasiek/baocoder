#!/usr/bin/env python3
"""Export the firmware's own ANALYSIS parameters as baocoder fixtures.

Reads the EmuRun output of tools/fw_oracle/gen_encode_jobs.py and writes:

  <name>.fwanalysis  one line per frame: class L f0(Q19) word-at-+0x08(hex)

                     The last column is NOT the voicing field.  On the decode
                     side +0x08 is the voicing word and test_firmware verifies
                     that decoding exactly, but on this path its bits do not
                     match the transmitted b1's codebook expansion (57%, barely
                     above chance) and it grows monotonically across frames in
                     a way that does not track L.  Nor does any offset in a
                     0x2000 window of the context hold that expansion, or the
                     pitch b0 decodes to - the encoder searches for indices
                     without materialising what they mean.  It is exported as
                     raw evidence, not as voicing.
  <name>.fwaenv      one line per frame: L int16 log2 amplitudes, Q11, the
                     analyser's own spectral envelope
  <name>.fwencbits   one line per frame: the 72 on-air bits
                     Vocoder_EncodeFrameParameters 0x0001994C emitted, as '0'
                     and '1' characters

This is the encode-side counterpart of export.py.  It matters because the
analysis stage is the one layer baocoder has no other reference for -
src/ambe_analysis.c is its own work rather than a transcription.

SPDX-License-Identifier: ISC
"""
import os, struct, sys

REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))
import emu


def export(cap, src=".", out="tests/fixtures"):
    res = emu.parse("%s/%s.out" % (src, cap))
    assert not res["faults"] and not res["errors"], (cap, res["faults"][:2])
    n = len([k for k in res["peek"] if k.startswith("apar")])

    with open("%s/%s.fwanalysis" % (out, cap), "w") as fh:
        fh.write("# firmware Vocoder_ProcessFrameFec 0x00016F3C under the p-code\n"
                 "# emulator, over %s.fwpcm - the radio's own analysis of that audio\n"
                 "# columns: frame-class  L  f0(Q19)  word-at-+0x08(hex)\n"
                 "# the last column is NOT the voicing field - see the module\n"
                 "# docstring and docs/encoder-parity.md\n" % cap)
        for f in range(n):
            p = res["peek"]["apar%d" % f][0]
            w = emu.u16(p, 8) | (emu.u16(p, 0x0A) << 16)
            fh.write("%d %d %d %08x\n" % (emu.u16(p, 0), emu.u16(p, 4),
                                          emu.u16(p, 0xC), w))

    with open("%s/%s.fwaenv" % (out, cap), "w") as fh:
        fh.write("# the analyser's own spectral envelope, log2 at Q11,\n"
                 "# from RX_CTX+0xABC+0x10 - what ambe_analyse should produce\n")
        for f in range(n):
            p = res["peek"]["apar%d" % f][0]
            L = emu.u16(p, 4)
            L = L if 0 < L <= 56 else 0
            fh.write(" ".join(str(emu.s16(p, 0x10 + 2 * k)) for k in range(L)) + "\n")

    with open("%s/%s.fwencbits" % (out, cap), "w") as fh:
        fh.write("# the 72 on-air bits Vocoder_EncodeFrameParameters emitted,\n"
                 "# one frame per line, before Vocoder_RxTask's XOR and descramble\n")
        for f in range(n):
            b = res["peek"]["bits%d" % f][0]
            fh.write("".join("1" if emu.u16(b, 2 * k) & 1 else "0"
                             for k in range(len(b) // 2)) + "\n")

    print("%s: %d frames -> .fwanalysis .fwaenv .fwencbits" % (cap, n))
    return n


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else "."
    out = sys.argv[2] if len(sys.argv) > 2 else "tests/fixtures"
    for cap in sys.argv[3:] or ["dm32_arc4_1"]:
        export(cap, src, out)
