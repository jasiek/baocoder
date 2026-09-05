#!/usr/bin/env python3
"""Oracle Vocoder_SynthesizeUnvoiced 0x0001AFE0 by CALLING it, one case at a time.

`gen_synth_jobs.py` captures the synthesiser inside a running Vocoder_TxTask,
which is the only way to get its *inputs*: they are channel state no caller can
set up from cold.  This is the other half.  Once those inputs are known they
can be poked back in and the function invoked directly, which is worth doing
for three reasons:

  it is a unit oracle - one call, one result, no sequence to keep aligned, so
  a fixture record cannot silently pair one call's state with another's block;

  it is fast - 103 calls in 27 seconds against ten minutes for the task run,
  because there is no RTOS, no FEC and no ConfigureFrame around it;

  the inputs can be *changed*.  The frame class selects between two quite
  different gain paths and this corpus only ever contains class 1, so the
  class-2 path is reachable here and nowhere else.

The function reads nothing but its five arguments and the SRAM constant pool,
so a direct call is faithful: pChannelState is not consulted, and the voicing
array the parameter block points at is supplied here rather than being built
by Vocoder_ResetFrameBuffer - which is what makes ambe_unvoiced_voicing()
testable against ambe_unvoiced_synth() rather than only through it.

Two breaks inside the call bracket the spectral shaping, so a failure lands on
a stage rather than on the whole function:

    0x0001B1D4  the Dsp_FftForward call - the windowed noise, complete
    0x0001B470  the Dsp_FftInverse call - the same buffer, shaped

Both read the caller's `local_2d8` at a fixed stack address, because the job
sets sp itself and the frame below it is deterministic; `sp` is read at each
stop and asserted rather than assumed.

    python3 tools/fw_oracle/gen_uv_jobs.py /tmp/uv.job
    EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/uv.job /tmp/uv.out
    python3 tools/fw_oracle/gen_uv_jobs.py --export /tmp/uv.out \
            tests/fixtures/dm32_arc4_1.fwunvoiced

REQUIRES the C-SKY sleigh patched with docs/patches/csky-mvcv.patch.  Without
it `mvcv` yields 0xFE/0xFF instead of 0/1, every branch on a condition moved
into a register goes the wrong way, and this function in particular takes the
class-2 gain path for every frame.

SPDX-License-Identifier: ISC
"""
import os, struct, sys

REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))
import emu

UV        = 0x0001AFE0
BRFP      = 0x00022CD0    # Vocoder_BuildFrameResetPattern
BRK_FFT   = 0x0001B1D4
BRK_IFFT  = 0x0001B470

# Scratch, well below the stack top and above .bss's tail.
ACC   = 0x00051000        # 80 int32, the accumulator the call adds into
STATE = 0x00051200        # 170 int16, pChannelState+0x648
BLK   = 0x00051400        # 68 int16, the parameter block
VOIC  = 0x00051500        # 0x38 uint16, the per-harmonic voicing flags

NS    = 0x50              # nCount, the only value the radio ever passes
NBLK  = 68
NST   = 170
NVOIC = 0x38

# The frame Vocoder_SynthesizeUnvoiced builds under emu.STACK_TOP, measured:
# `addi r10,sp,0x314` at 0x0001B25E writes the exponent scratch the decompiler
# calls local_28, which is 0x28 below the frame base, so the frame is 0x33C and
# local_2d8 - the 256-point transform buffer - sits 0x64 above sp.
FRAME    = 0x344
BUF_OFF  = 0x64
BUF      = emu.STACK_TOP - FRAME + BUF_OFF
BUF_LEN  = 512


def _rows(path):
    out = []
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        out.append([int(x) for x in line.split()])
    return out


def _s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def gen(jobfile, src):
    """One call per record of `src`, whose first 1+NBLK+NST columns are pitch,
    parameter block and state - the layout gen_synth_jobs.py's capture and this
    fixture share."""
    rows = _rows(src)
    j = emu.Job()
    j.hook_break(BRK_FFT)
    j.hook_break(BRK_IFFT)
    for i, r in enumerate(rows):
        pitch = r[0]
        blk   = list(r[1:1 + NBLK])
        st    = r[1 + NBLK:1 + NBLK + NST]
        blk[0x40] = VOIC & 0xFFFF          # the block's pointer to the flags
        blk[0x41] = (VOIC >> 16) & 0xFFFF
        word  = ((blk[5] & 0xFFFF) << 16) | (blk[4] & 0xFFFF)
        j.poke(ACC,   b"\x00" * (NS * 4))
        j.poke(STATE, struct.pack("<%dh" % NST, *[_s16(x) for x in st]))
        j.poke(VOIC,  b"\xEE" * (NVOIC * 2))
        j.poke(BLK,   struct.pack("<%dh" % NBLK, *[_s16(x) for x in blk]))
        # the firmware builds the flags itself, from the block it was handed
        j.call(BRFP, VOIC, word, blk[6] & 0xFFFF, blk[2])
        j.peek("voi%d" % i, VOIC, NVOIC * 2)
        j.call(UV, ACC, NS, STATE, BLK, pitch)
        j.getreg("sp"); j.peek("wnd%d" % i, BUF, BUF_LEN); j.resume()
        j.getreg("sp"); j.peek("shp%d" % i, BUF, BUF_LEN); j.resume()
        j.peek("acc%d" % i, ACC, NS * 4)
        j.peek("st%d"  % i, STATE, NST * 2)
    j.write(jobfile)
    print("wrote %s: %d calls" % (jobfile, len(rows)))
    return len(rows)


def export(outfile, src, dest):
    """Rewrite `src`'s output columns from a completed run."""
    rows = _rows(src)
    res = emu.parse(outfile)
    assert not res["faults"], res["faults"][:2]
    assert not res["errors"], res["errors"][:2]
    bad = [v for n, v in res["regs"] if n == "sp" and v != emu.STACK_TOP - FRAME]
    assert not bad, "stack frame moved: sp=%08x, expected %08x" % (
        bad[0], emu.STACK_TOP - FRAME)
    n = 0
    with open(dest, "w") as fh:
        fh.write("# Vocoder_SynthesizeUnvoiced 0x0001AFE0, called directly under the\n"
                 "# p-code emulator over the inputs Vocoder_TxTask produced for\n"
                 "# tests/fixtures/dm32_arc4_1.frames.  tools/fw_oracle/gen_uv_jobs.py.\n"
                 "# per record: pitch, %d shorts of parameter block, %d shorts of state\n"
                 "#   before, the %d per-harmonic voicing flags\n"
                 "#   Vocoder_BuildFrameResetPattern 0x00022CD0 built from that block,\n"
                 "#   %d shorts of the shaped spectrum handed to Dsp_FftInverse, the %d\n"
                 "#   int32 samples added to the accumulator, then %d shorts of state after\n"
                 % (NBLK, NST, NVOIC, 256, NS, NST))
        for i, r in enumerate(rows):
            head = r[:1 + NBLK + NST]
            voi = struct.unpack("<%dH" % NVOIC, res["peek"]["voi%d" % i][0])
            shp = struct.unpack("<256h", res["peek"]["shp%d" % i][0])
            acc = struct.unpack("<%di" % NS, res["peek"]["acc%d" % i][0])
            st1 = struct.unpack("<%dh" % NST, res["peek"]["st%d" % i][0])
            fh.write("%s %s %s %s %s\n" % (
                " ".join(map(str, head)), " ".join(map(str, voi)),
                " ".join(map(str, shp)), " ".join(map(str, acc)),
                " ".join(map(str, st1))))
            n += 1
    print("%s: %d calls" % (dest, n))
    return n


if __name__ == "__main__":
    if sys.argv[1] == "--export":
        export(sys.argv[2], sys.argv[3], sys.argv[3])
    else:
        gen(sys.argv[1], sys.argv[2] if len(sys.argv) > 2
            else "tests/fixtures/dm32_arc4_1.fwunvoiced")
