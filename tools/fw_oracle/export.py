#!/usr/bin/env python3
"""Export the firmware's own decoded parameters as baocoder fixtures.

Reads the EmuRun output of tools/emu driving Vocoder_TxTask 0x0002EC30 over the
capture's payloads and writes, per capture:

  <name>.fwparms   one line per frame: class L f0 voicing-word(hex)
  <name>.fwamps    one line per frame: L int16 spectral amplitudes (params+0x10)
  <name>.fwpcm     raw int16 PCM, 160 samples per frame

The parameter block layout is Vocoder_CopyFrameParamsWithReset 0x00019CBC's:
+0x00 frame class, +0x04 L, +0x08 voicing (uint32 with +0x0A), +0x0C f0 Q19,
+0x10 a 0x38-short array of spectral amplitudes.
"""
import os, sys
REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))
import emu

S = sys.argv[1] if len(sys.argv) > 1 else "."
OUT = sys.argv[2] if len(sys.argv) > 2 else "tests/fixtures"
HALF = 0x50
RING = 0xA0

def export(cap, amps=False, pcm=False):
    res = emu.parse("%s/%s.out" % (S, cap))
    assert not res["faults"] and not res["errors"], (cap, res["faults"][:2])
    n = len([k for k in res["peek"] if k.startswith("par")])
    with open("%s/%s.fwparms" % (OUT, cap), "w") as fh:
        fh.write("# firmware Vocoder_TxTask 0x0002EC30 under the p-code emulator,\n"
                 "# same 49-bit payloads as %s.ambe49\n"
                 "# columns: frame-class  L  f0(Q19)  voicing-word(hex, 16 crumbs)\n" % cap)
        for f in range(n):
            p = res["peek"]["par%d" % f][0]
            w = emu.u16(p, 8) | (emu.u16(p, 0x0A) << 16)
            fh.write("%d %d %d %08x\n" % (emu.u16(p, 0), emu.u16(p, 4), emu.u16(p, 0xC), w))
    written = ["%s.fwparms" % cap]
    if amps:
        with open("%s/%s.fwamps" % (OUT, cap), "w") as fh:
            fh.write("# firmware spectral amplitudes, params+0x10, int16 block float\n"
                     "# one line per frame: L values\n")
            for f in range(n):
                p = res["peek"]["par%d" % f][0]
                L = emu.u16(p, 4)
                fh.write(" ".join(str(emu.s16(p, 0x10 + 2 * k)) for k in range(L)) + "\n")
        written.append("%s.fwamps" % cap)
    if pcm:
        buf = bytearray()
        nw = len([k for k in res["peek"] if k.startswith("pcm")])
        for w in range(nw):
            block = res["peek"]["pcm%d" % w][0]
            idx = emu.s16(res["peek"]["ix%d" % w][0], 0)
            slot = (idx - HALF) % RING
            buf += block[slot * 2:(slot + HALF) * 2]
        open("%s/%s.fwpcm" % (OUT, cap), "wb").write(bytes(buf))
        written.append("%s.fwpcm (%d samples)" % (cap, len(buf) // 2))
    print("%-14s %d frames -> %s" % (cap, n, ", ".join(written)))

if __name__ == "__main__":
    for cap in ["dm32_arc4_1", "dm32_arc4_2", "dm32_aes128_1",
                "dm32_aes128_2", "dm32_aes256_1", "dm32_aes256_2"]:
        export(cap, amps=(cap == "dm32_arc4_1"), pcm=(cap == "dm32_arc4_1"))
