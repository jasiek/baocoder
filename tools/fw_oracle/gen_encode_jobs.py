#!/usr/bin/env python3
"""Generate an EmuRun job that ENCODES a PCM file through the firmware.

The decode-side oracle (gen_jobs.py) drives Vocoder_TxTask.  This one drives the
transmit path, whose entry point is Vocoder_ProcessFrameFec 0x00016F3C:

    Vocoder_ProcessFrame(pCtx, pSamples, nFrameSize, wFlags, bQuantise)
    if (bQuantise & 1) Vocoder_EncodeFrameParameters(pDest, ...)

A 20 ms frame is two calls of 80 samples, the first with bQuantise = 0 and the
second with 1, which is exactly what Vocoder_RxTask 0x0002E5A0 does - it passes
`0x50, 0x800, 1` and `0x50, 0x800, 0`, so the argument values here are the
radio's own rather than a guess.

Two things are peeked per frame:

  the analysis parameters at RX_CTX+0xABC, whose layout is the same
  Vocoder_CopyFrameParamsWithReset 0x00019CBC block the decode side uses -
  +0x00 class, +0x02 the 0xFF marker, +0x04 L, +0x08 voicing u32, +0x0C f0 in
  Q19, +0x10 the log2 envelope at Q11.  This is the layer baocoder has no other
  reference for: src/ambe_analysis.c is its own work.

  the 72 on-air bits Vocoder_EncodeFrameParameters emits.

The block was located by scanning a 0x2000 window of g_VocoderRxCtx for that
signature across 40 frames, not by reading the decompiler - AnalyzeSpectrum is
handed pCtx+0x938 and pCtx+0xB50, and it is neither of those.

SPDX-License-Identifier: ISC
"""
import os, sys

REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))
import emu, vocoder_ctx as V
import oracle_encode as E

APARMS = 0xABC       # the analysis parameter block, relative to RX_CTX
APARMS_LEN = 0x90


def gen(jobfile, pcmfile, nframes):
    halves = E.read_pcm(pcmfile, nframes)
    j = emu.Job(); V.init(j)
    j.comment("encode %d frames through Vocoder_ProcessFrameFec" % nframes)
    for i in range(nframes):
        j.poke(E.PCM_IN, halves[2 * i])
        j.call(E.PROCESS_FRAME_FEC, E.OUT_BITS, E.PARAM_2, E.PCM_IN, E.HALF,
               E.WFLAGS, 0, E.PARAM_7, E.CTX)
        j.poke(E.PCM_IN, halves[2 * i + 1])
        j.call(E.PROCESS_FRAME_FEC, E.OUT_BITS, E.PARAM_2, E.PCM_IN, E.HALF,
               E.WFLAGS, 1, E.PARAM_7, E.CTX)
        j.peek("apar%d" % i, E.CTX + APARMS, APARMS_LEN)
        j.peek("bits%d" % i, E.OUT_BITS, E.NBITS * 2)
    j.write(jobfile)
    print("wrote %s: %d frames" % (jobfile, nframes))


if __name__ == "__main__":
    gen(sys.argv[1], sys.argv[2], int(sys.argv[3]))
