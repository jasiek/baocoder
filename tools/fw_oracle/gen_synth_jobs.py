#!/usr/bin/env python3
"""Capture every Vocoder_SynthesizeFrame 0x00019DB8 call: its input block and
the 80 samples it produced.

The synthesiser cannot be oracled one call at a time.  Its output depends on
channel state spread across pChannelState +0x18 (the voiced synthesiser's phase
and history), +0x470 (the previous frame's parameters), +0x648 (the unvoiced
side's state) and +0x7be (the smoothed pitch), none of which a caller can set
up meaningfully from cold.  So this captures SEQUENCES: a transcription that
carries its own state is run over the same inputs and diffed call by call, the
first divergence being the thing to fix.

Two breaks:

  0x00019DB8  entry.  r0 is the parameter block - 0x00057E14 for the first
              80-sample half of a frame (the interpolated block) and
              0x00045AA4 for the second (PARAMS+0x000); r3 is bRepeatFrame.
              Both candidates are peeked and r0 says which was used.
  0x00016D5A  where Vocoder_ConfigureFrame's two call sites converge - the
              first branches here, the second falls through - so one address
              catches every return.  The destination r1 is TX_CTX+0x116E, the
              PCM ring, so the samples are at a fixed address.

SPDX-License-Identifier: ISC
"""
import os, struct, sys

REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))
import emu, vocoder_ctx as V
import oracle_dectask as D

BRK_SYNTH_IN  = 0x00019DB8
BRK_SYNTH_OUT = 0x00016D5A
# the three sample-domain stages, snapshotted so each can be transcribed and
# checked on its own rather than only end to end
BRK_AT_UV     = 0x00019ECE     # the call itself: the parameter block is fully
                               # preprocessed here, which it is not at entry
BRK_AFTER_UV  = 0x00019ED2     # after Vocoder_SynthesizeUnvoiced 0x0001AFE0
BRK_AFTER_V   = 0x00019EF2     # after Vocoder_SynthesizeVoiced   0x0001DE10
BRK_AFTER_PF  = 0x00019EFE     # after FUN_00018a2c
# aiStack_338, measured at all three call sites: Vocoder_SynthesizeUnvoiced,
# Vocoder_SynthesizeVoiced and FUN_00018a2c are all handed 0x00057AD0, which is
# what the decompiler says.  An earlier recon of mine read 0x00057B10 for the
# unvoiced call and I wrongly concluded the decompiler had mismarshalled the
# arguments; re-measuring all three together disagrees with that reading, and
# the postfilter fixture is bit-exact at 0x00057AD0 over 13 280 samples.
ACC           = 0x00057AD0
ACC_LEN       = 100 * 4
BLK_INTERP    = 0x00057E14     # the interpolated block, first half
BLK_PARAMS    = 0x00045AA4     # PARAMS+0x000, second half
BLK_LEN       = 0x100
RING          = V.TX_CTX + D.PCM_RING
RING_LEN      = D.PCM_RING_LEN * 2
# The voiced synthesiser's two remaining arguments.  Nothing between
# BRK_AFTER_UV 0x00019ED2 and the call at 0x00019EEE writes either of them - the
# twelve instructions in between are loads, moves and two stack stores, which is
# the whole reason no new break is needed here.  Adding one would shift the stop
# sequencing this job's frame feed is keyed to; adding a peek cannot.
V_STATE       = V.TX_CTX + 0x18    # param_3, pChannelState+0x18
N_VST         = 0x22C              # shorts, the WHOLE span from pState to PREV.
                                   # The function's own accesses off r11 stop at
                                   # byte 0x1ca (short 0xe5) and it hands four
                                   # pointers to helpers - shorts 0xab, 0xad,
                                   # 0xe5, 0xe6 - so how far the last array runs
                                   # is the helpers' business, not visible here.
                                   # A first attempt at 0x140 guessed 0x38 for
                                   # it, the length Vocoder_SynthesizeFrame
                                   # clears at ctx+0x172 (which is pState+0xad).
                                   # 337 of 617 direct calls then died on a
                                   # divide by zero at 0x00018F14, and the ones
                                   # that died were the ones with live data at
                                   # the edge of the window.  The array at
                                   # [0xe6] is not 0x38 long: it is the harmonic
                                   # SPECTRUM, and Vocoder_SynthesizeHarmonicSpectrum
                                   # 0x0001D4C8 clears it 0x80<<1 = 0x100 shorts
                                   # at a time before handing it to
                                   # Dsp_FftInverse 0x00025704, so the state runs
                                   # to at least 0xe6+0x100 = 0x1e6.  0x22C
                                   # reaches the next structure the memory map
                                   # knows about and ends the guessing.
PREV          = V.TX_CTX + 0x470   # param_5, the previous frame's parameters:
                                   # `addi r0,r6,0x470 / st.w r0,(sp,0x8)` at
                                   # 0x00019DC6, reloaded into r13 at 0x00019ED6
# The per-harmonic voicing flags.  pParams[0x40..0x41] is a POINTER to them and
# both synthesisers dereference it, so a fixture that omits the array is not a
# complete set of inputs.  They are NOT in the channel state: measured over 345
# captured calls the pointer holds exactly one value from the current block,
# 0x00057D70, and exactly one from the previous, 0x00057D00 - two adjacent
# scratch arrays 0x70 bytes apart, which is 0x38 shorts, which is the harmonic
# maximum and the length Vocoder_BuildFrameResetPattern fills.  One peek covers
# both; export_voiced locates the current frame's within it from the pointer
# rather than trusting the constant.
VOI           = 0x00057D00
N_VOI         = 0x38               # shorts per array
VOI_LEN       = N_VOI * 2 * 2      # both arrays


def gen(jobfile, framesfile, nframes):
    frames = D.read_frames(framesfile, nframes, 0)
    j = emu.Job(); V.init(j)
    for addr in D.STUBS:
        j.hook_ret(addr, 0)
    j.hook_break(D.BRK_AFTER_PEND)
    j.hook_break(BRK_SYNTH_IN)
    j.hook_break(BRK_SYNTH_OUT)
    for a in (BRK_AT_UV, BRK_AFTER_UV, BRK_AFTER_V, BRK_AFTER_PF):
        j.hook_break(a)
    j.poke(V.TX_CTX + D.F_GATE, b"\x01")
    j.poke(V.TX_CTX + D.F_MODE, b"\x00" * D.N_MODE)
    j.call(D.TX_TASK, D.SCRATCH_ERR)
    for w in range(nframes * D.WAKES_PER_FRAME * 6):
        f = (w // 6) // D.WAKES_PER_FRAME
        if f < nframes:
            j.poke(V.TX_CTX + D.F_FILL, struct.pack("<h", D.HALF))
            j.poke(V.TX_CTX + D.F_BURST + (f % 3) * 9, frames[f])
        j.resume()
        for r in ("r0", "r1", "r3"):
            j.getreg(r)
        j.peek("bi%d" % w, BLK_INTERP, BLK_LEN)
        j.peek("bp%d" % w, BLK_PARAMS, BLK_LEN)
        j.peek("ring%d" % w, RING, RING_LEN)
        j.peek("acc%d" % w, ACC, ACC_LEN)
        # FUN_00018a2c's six state words live at the very start of the channel
        # context; without them its first output cannot be reproduced
        j.peek("fst%d" % w, V.TX_CTX, 0x18)
        # the unvoiced synthesiser's own state, param_3 = pChannelState+0x648,
        # which the code indexes up to [0xa9]
        j.peek("uvs%d" % w, V.TX_CTX + 0x648, 0x160)
        # the smoothed pitch it is passed, and the block it reads
        j.peek("pit%d" % w, V.TX_CTX + 0x7be, 2)
        # the voiced synthesiser's state, param_3 = pChannelState+0x18, and the
        # previous frame's parameter block it is handed as param_5
        j.peek("vst%d" % w, V_STATE, N_VST * 2)
        j.peek("prv%d" % w, PREV, BLK_LEN)
        j.peek("voi%d" % w, VOI, VOI_LEN)
    j.write(jobfile)
    print("wrote %s: %d stops" % (jobfile, nframes * D.WAKES_PER_FRAME * 6))


if __name__ == "__main__":
    gen(sys.argv[1], sys.argv[2], int(sys.argv[3]))


def export(outfile, dest):
    """Write one record per Vocoder_SynthesizeFrame call.

    Fields: source (0 = the interpolated block, 1 = PARAMS+0x000),
    bRepeatFrame, the destination offset into the ring in samples, 68 shorts of
    the input parameter block, then the 80 samples the call produced.

    pFrameParams[0x40..0x41] is a scratch POINTER the firmware plants before
    calling Vocoder_ResetFrameBuffer, so its value is emulator-local and means
    nothing to a reimplementation; it is recorded rather than filtered because
    a fixture should be what the machine held, not what I think matters.
    """
    stops, cur = [], None
    for line in open(outfile):
        line = line.strip()
        if line.startswith("BREAK "):
            cur = {"pc": int(line.split()[1], 16), "r": {}, "p": {}}
            stops.append(cur)
        elif line.startswith("REG ") and cur is not None:
            _, n, h = line.split()
            cur["r"][n] = int(h, 16)
        elif line.startswith("PEEK ") and cur is not None:
            _, n, h = line.split()
            cur["p"][n.rstrip("0123456789")] = bytes.fromhex(h)

    ins = [i for i, s in enumerate(stops) if s["pc"] == BRK_SYNTH_IN]
    n = 0
    with open(dest, "w") as fh:
        fh.write("# Vocoder_SynthesizeFrame 0x00019DB8, every call over the capture.\n"
                 "# per record: src(0=interpolated 1=PARAMS+0) repeat dstOffset\n"
                 "#             68 shorts of the input block, then 80 output samples\n")
        for i in ins:
            j = i + 1
            while j < len(stops) and stops[j]["pc"] != BRK_SYNTH_OUT:
                j += 1
            if j >= len(stops):
                break
            r = stops[i]["r"]
            src = 0 if r["r0"] == BLK_INTERP else 1
            blk = stops[i]["p"]["bi" if src == 0 else "bp"]
            off = r["r1"] - RING
            out = stops[j]["p"]["ring"][off:off + 160]
            fh.write("%d %d %d %s %s\n" % (
                src, r["r3"], off // 2,
                " ".join(str(x) for x in struct.unpack_from("<68h", blk, 0)),
                " ".join(str(x) for x in struct.unpack("<80h", out))))
            n += 1
    print("%s: %d synthesiser calls" % (dest, n))
    return n


def export_postfilter(outfile, dest):
    """Write FUN_00018a2c's input, output and state for every call.

    The state is recorded per call as well as the samples, so the test can
    check two different things: that the filter is right given the firmware's
    state, and that it PREDICTS that state correctly when carrying its own.  A
    filter can pass the first and fail the second.
    """
    stops, cur = [], None
    for line in open(outfile):
        line = line.strip()
        if line.startswith("BREAK "):
            cur = {"pc": int(line.split()[1], 16), "p": {}, "r": {}}
            stops.append(cur)
        elif line.startswith("PEEK ") and cur is not None:
            _, n, h = line.split()
            cur["p"][n.rstrip("0123456789")] = bytes.fromhex(h)
        elif line.startswith("REG ") and cur is not None:
            _, n, h = line.split()
            cur["r"][n] = int(h, 16)

    n = 0
    with open(dest, "w") as fh:
        fh.write("# FUN_00018a2c, the output filter Vocoder_SynthesizeFrame runs between\n"
                 "# the harmonic synthesis and the final scaling, captured either side.\n"
                 "# per record: 6 state words before, 80 input ints, 80 output ints,\n"
                 "#             then the 80 int16 PCM samples the call finally wrote\n")
        for i, s in enumerate(stops):
            if s["pc"] != BRK_AFTER_V:
                continue
            j = i + 1
            while j < len(stops) and stops[j]["pc"] != BRK_AFTER_PF:
                j += 1
            if j >= len(stops):
                break
            k = i - 1
            while k >= 0 and stops[k]["pc"] != BRK_SYNTH_IN:
                k -= 1
            m = j + 1
            while m < len(stops) and stops[m]["pc"] != BRK_SYNTH_OUT:
                m += 1
            if k < 0 or m >= len(stops):
                break
            off = stops[k]["r"]["r1"] - RING
            st = struct.unpack("<6i", s["p"]["fst"])
            a = struct.unpack_from("<80i", s["p"]["acc"], 0)
            b = struct.unpack_from("<80i", stops[j]["p"]["acc"], 0)
            pcm = struct.unpack_from("<80h", stops[m]["p"]["ring"], off)
            fh.write("%s %s %s %s\n" % (" ".join(map(str, st)),
                                        " ".join(map(str, a)),
                                        " ".join(map(str, b)),
                                        " ".join(map(str, pcm))))
            n += 1
    print("%s: %d filter calls" % (dest, n))
    return n


def export_unvoiced(outfile, dest):
    """Write Vocoder_SynthesizeUnvoiced's inputs, output and state per call.

    The accumulator is zeroed immediately before this call, so what it leaves
    there is the unvoiced contribution alone - the function can be checked on
    its own without disentangling it from the voiced path.

    The parameter block is peeked at 0x00019ECE, the call instruction itself,
    not at the function's entry: Vocoder_SynthesizeFrame rewrites that block in
    place on the way down (normalisation, excitation match, the gain ramp), so
    the version at entry is not the version the synthesiser sees.
    """
    stops, cur = [], None
    for line in open(outfile):
        line = line.strip()
        if line.startswith("BREAK "):
            cur = {"pc": int(line.split()[1], 16), "p": {}, "r": {}}
            stops.append(cur)
        elif line.startswith("PEEK ") and cur is not None:
            _, n, h = line.split()
            cur["p"][n.rstrip("0123456789")] = bytes.fromhex(h)
        elif line.startswith("REG ") and cur is not None:
            _, n, h = line.split()
            cur["r"][n] = int(h, 16)

    n = 0
    with open(dest, "w") as fh:
        fh.write("# Vocoder_SynthesizeUnvoiced 0x0001AFE0, every call over the capture.\n"
                 "# per record: pitch, 68 shorts of the parameter block as the call sees\n"
                 "#   it, 170 shorts of state (pChannelState+0x648) before, then the 80\n"
                 "#   int32 samples produced, then 170 shorts of state after\n")
        for i, s in enumerate(stops):
            if s["pc"] != BRK_AT_UV:
                continue
            j = i + 1
            while j < len(stops) and stops[j]["pc"] != BRK_AFTER_UV:
                j += 1
            if j >= len(stops):
                break
            blk = s["p"]["bi" if s["r"].get("r3") == BLK_INTERP else "bp"]
            pit = struct.unpack("<h", s["p"]["pit"])[0]
            st0 = struct.unpack_from("<170h", s["p"]["uvs"], 0)
            st1 = struct.unpack_from("<170h", stops[j]["p"]["uvs"], 0)
            acc = struct.unpack_from("<80i", stops[j]["p"]["acc"], 0)
            fh.write("%d %s %s %s %s\n" % (
                pit,
                " ".join(map(str, struct.unpack_from("<68h", blk, 0))),
                " ".join(map(str, st0)),
                " ".join(map(str, acc)),
                " ".join(map(str, st1))))
            n += 1
    print("%s: %d unvoiced calls" % (dest, n))
    return n


def export_voiced(outfile, dest):
    """Write Vocoder_SynthesizeVoiced's six arguments, output and state per call.

    Unlike the unvoiced call the accumulator is NOT zero here - the unvoiced
    contribution is already in it - so the record carries the accumulator on
    both sides rather than only after.  The function adds into it
    (`acc[i] = (buf[i] + acc[i]) - ramp*buf[i]`, and the subtracted term depends
    only on its own buffer), so the voiced contribution is separable by
    subtraction; it is recorded unseparated because a fixture should be what the
    machine held.

    Everything is read at BRK_AFTER_UV, twelve instructions before the call.
    That is deliberate: `ld.h r12,(r6,0x7be) / ld.w r13,(sp,0x8) / sexth /
    mov r0,r5 / mov r1,r7 / addi r2,r6,0x18 / mov r3,r4 / st.w r13,(sp,0x0) /
    st.w r12,(sp,0x4)` is the whole of it, and none of it writes the state, the
    blocks or the pitch.  So the inputs are complete there, and no break has to
    be added at 0x00019EEE itself - which matters, because this job's frame feed
    is keyed to the number of stops per wake.

    r3 is not read back for the block source: it is caller-saved and the
    unvoiced call has just clobbered it.  The enclosing BRK_SYNTH_IN stop's r0
    is what says which block, the same walk-back export_postfilter does.
    """
    stops, cur = [], None
    for line in open(outfile):
        line = line.strip()
        if line.startswith("BREAK "):
            cur = {"pc": int(line.split()[1], 16), "p": {}, "r": {}}
            stops.append(cur)
        elif line.startswith("PEEK ") and cur is not None:
            _, n, h = line.split()
            cur["p"][n.rstrip("0123456789")] = bytes.fromhex(h)
        elif line.startswith("REG ") and cur is not None:
            _, n, h = line.split()
            cur["r"][n] = int(h, 16)

    n = 0
    with open(dest, "w") as fh:
        fh.write("# Vocoder_SynthesizeVoiced 0x0001DE10, every call over the capture.\n"
                 "# Vocoder_SynthesizeFrame hands it (acc, n, pChannelState+0x18,\n"
                 "#   pParams, pPrevParams, nPitch) - r0..r3 and two stack slots.\n"
                 "# per record: pitch, %d shorts of the current parameter block as the\n"
                 "#   call sees it, %d shorts of the previous block (ctx+0x470), %d\n"
                 "#   shorts of state before, the %d per-harmonic voicing flags\n"
                 "#   pParams[0x40] points at, the %d int32 accumulator before (the\n"
                 "#   unvoiced contribution) and after, then %d shorts of state after\n"
                 % (68, 68, N_VST, N_VOI, 80, N_VST))
        for i, s in enumerate(stops):
            if s["pc"] != BRK_AFTER_UV:
                continue
            j = i + 1
            while j < len(stops) and stops[j]["pc"] != BRK_AFTER_V:
                j += 1
            if j >= len(stops):
                break
            k = i - 1
            while k >= 0 and stops[k]["pc"] != BRK_SYNTH_IN:
                k -= 1
            if k < 0:
                break
            blk = s["p"]["bi" if stops[k]["r"]["r0"] == BLK_INTERP else "bp"]
            # where the block says its voicing flags are, not where they were
            # last time: an address that has moved outside the peeked window is
            # a fixture with a hole in it, so stop rather than slice blindly
            ptr = struct.unpack_from("<I", blk, 0x80)[0]
            off = ptr - VOI
            assert 0 <= off and off + N_VOI * 2 <= VOI_LEN, (
                "pParams[0x40] is %08x, outside the %d bytes peeked at %08x"
                % (ptr, VOI_LEN, VOI))
            fh.write("%d %s %s %s %s %s %s %s\n" % (
                struct.unpack("<h", s["p"]["pit"])[0],
                " ".join(map(str, struct.unpack_from("<68h", blk, 0))),
                " ".join(map(str, struct.unpack_from("<68h", s["p"]["prv"], 0))),
                " ".join(map(str, struct.unpack_from("<%dh" % N_VST, s["p"]["vst"], 0))),
                " ".join(map(str, struct.unpack_from("<%dH" % N_VOI, s["p"]["voi"], off))),
                " ".join(map(str, struct.unpack_from("<80i", s["p"]["acc"], 0))),
                " ".join(map(str, struct.unpack_from("<80i", stops[j]["p"]["acc"], 0))),
                " ".join(map(str, struct.unpack_from("<%dh" % N_VST, stops[j]["p"]["vst"], 0)))))
            n += 1
    print("%s: %d voiced calls" % (dest, n))
    return n
