# The amplitude gap — resolved, and what replaced it

The "1.15 dB spectral amplitude gap" was a **comparison error, not a decoder
error**. The fixture was reading the wrong parameter block.

## What it actually is

`tests/fixtures/*.fwamps` was exported from the block at `params+0x10`, on the
belief that it was the envelope after interpolation. It is neither the envelope
nor interpolated. Breaking inside the interpolator (`tools/fw_oracle/resample_probe.py`)
settles both halves:

* **`Vocoder_ResampleSpectralEnvelope 0x00026A84` does not write into the
  parameter context at all.** Its output pointer is `0x00057E14`, a *caller's
  stack local*. No block the corpus export can peek is its result.
* **`params+0x10` changes format between the two wakes of a frame.** At the
  interpolator it holds the frame's envelope, log2 at Q11. One wake later it has
  been converted in place to linear block-float amplitudes — enhanced,
  unvoiced-scaled and exponentiated. Measured on the same frame:
  `[6838, 6940, 7961, ...]` becomes `[19948, 20649, 29171, ...]`, and
  `2^(7961/2048)/2^(6838/2048) = 1.462` against `29171/19948 = 1.462`.

So `.fwamps` and `.fwenv` are 13.8 dB apart because they are different
variables in different units, not because anything decodes differently. The
1.15 dB was a comparison between `log2Ml` and a linear `Ml` array taken three
stages further down the pipeline.

The frame's own decoded envelope is in the **fourth** parameter block, at
`params+0x198` — `class +0`, `L +4`, `f0 +0xC`, envelope `+0x10`, log2 at Q11.
Against that, over **every voice frame of all six captures**:

| | |
|---|---|
| mean per-frame residual | **0.0034 log2 = 0.021 dB** |
| worst frame | 0.279 dB |

That is `tests/fixtures/*.fwenv`, and `test_firmware` now checks it. The
envelope decode in `src/ambe_params.c` is the radio's, confirmed by execution.

## How that was found

Two stubs and a fixed point. `Vocoder_NormalizeSpectralBlock 0x00022C18` is the
log2 -> linear stage; stub it and `Vocoder_MatchExcitationEnergy 0x000277F8` and
the blocks stay in the log2 domain, where they can be compared against
`ambe_parms.log2Ml` harmonic by harmonic. Feed one voice payload 24 times and the
envelope predictor reaches its fixed point, so no transient is in the way.

Then scan **every** block in the peeked window rather than assuming which one is
which:

| block | vs `log2Ml`, same frame | vs `log2Ml`, previous frame |
|---|--:|--:|
| `+0x010` | 3.19 dB | 4.49 dB |
| `+0x1a8` | **0.020 dB** | 4.05 dB |

## The chain, read

Ghidra's plugin was up, so the decode path was read rather than inferred.
`Vocoder_DecodeAmbeFrame 0x0002033C` reads its fields in exactly the order the
one-hot permutation probe implied — `b0`(7) `b1`(5) [`b2`(5), inside
`Vocoder_LookupAndBlendGain`] `b3`(9) `b4`(7) `b5`(5) `b6`(4) `b7`(4) `b8`(3) —
which is an independent confirmation of the field-ordered buffer layout.

| | |
|---|---|
| `Vocoder_CodeSpectralCoefficients 0x000220D4` | the envelope codec, both directions. Read end to end; agrees with `src/ambe_params.c` at every step |
| `Vocoder_CodebookVectorLookup 0x0002369C` | reads an index and copies a vector. It takes **both** the mode descriptor's field width and the codebook's natural width, and when they differ it reads the shorter index and strides the table by `len << (natural - actual)`. For 2450 they are equal for every field, so it reduces to `table[b * len]` |
| `Dsp_ResampleInterpolate 0x0002685C` | the DCT — a phase accumulator over `g_awSineTable512`, `a0 = 1`, `ak = 2`, stepping `floor(32768/n)` from SRAM `0x18003840` |
| `Dsp_HilbertTransform 0x00029D1C` | reads the log2 envelope, does not modify it |
| `Vocoder_NormalizeSpectralBlock 0x00022C18` | log2 -> linear: clamp to +-15 at Q11, block exponent `floor(max)+1`, `Math_Pow2Scaled` per harmonic, exponent to `+0x84` |
| `Math_Pow2Scaled 0x00019280` | the same Taylor core and the same SRAM `0x1800160C` coefficients `ambe_basop.c` uses, with a caller-chosen output Q |
| `Vocoder_MatchExcitationEnergy 0x000277F8` | the enhancement; voice frames only, and **stateful** (two words at `pChannelState+0x7c0`) |
| `FUN_00029914 0x00029914` | an adaptive smoother that pulls each harmonic toward the previous frame's resampled envelope with a frequency-dependent limit — **gated off** in this configuration; ablating it changes nothing |

## Vocoder_ResampleSpectralEnvelope, executed

The function is now transcribed and **verified bit-exactly against 232 live
calls** rather than read. `tools/fw_oracle/resample_probe.py` arms a break at
`0x00026BB4` — after the mix loop, where the output and both scratch arrays are
simultaneously live — and re-derives every stage from the peeked bytes:

```
interpolating calls captured: 232
  pitch branch  {'param_2 f0': 148, 'param_3 f0': 31, 'geometric mean': 53}
  f0_out as predicted            232/232
  local_104 reproduced bit-exact 232/232
  local_94  reproduced bit-exact 232/232
  out == clamp((l104+l94)/2)     232/232
```

Its three arguments, measured from the registers at the function's entry:

| | |
|---|---|
| `param_1` | `0x00057E14`, a caller's stack local — **the output** |
| `param_2` | `0x00045AA4` = `PARAMS+0x000`, the frame just decoded |
| `param_3` | `0x00045C3C` = `PARAMS+0x198`, the retained previous frame |

`param_3` at one call is `param_2` at the previous one: immediately after the
interpolation, `PARAMS+0x198` receives a byte-exact copy of `PARAMS+0x000`
(232/232), which is `FUN_00022024 0x00022024`. The two blocks are a delay line.

The algorithm:

1. **Pick a pitch**, from the two voicing words against the masks at
   `0x00026C64` (`0x55555555`) and `0x00026C68` (`0xAAAAAAAA`) — alternate bit
   planes of the packed voicing field, the same interleave
   `ambe_pitchless_gain_mode()` keys off. Either frame having no voiced energy
   selects the other frame's `f0` outright; both voiced gives
   `Math_SqrtScaled(2*f0cur*f0prev)`, a geometric mean. Measured split: 148
   current, 31 previous, 53 geometric mean, with zero mispredictions.
2. **`Vocoder_HarmonicCountFromPitch 0x0002AD18`** of that pitch gives the
   output `L`. On the geometric-mean branch this is a harmonic count belonging
   to *neither* input frame.
3. **Resample both envelopes onto that grid** with
   `Vocoder_ComputeHarmonicResampleRatio 0x000269B0`. It builds a 60-entry
   window — `buf[0] = src[0]`, `buf[k] = src[k-1]` for `k = 1..56`, three
   edge-holds above — so index `k` *is* harmonic `k` and the inner loop needs no
   bounds test. `Math_DivideNormalized 0x0002692C` gives `f0_out/f0_src` in Q16
   (short-circuiting to `0x10000` when equal), and that ratio is accumulated
   once per output harmonic:
   **`out[l] = src at harmonic position l * f0_out / f0_src`**. Frequency
   matched — a different law from the envelope predictor's `prevL/curL` *index*
   ratio in `src/ambe_params.c`. The interpolation weight is `0x7FFF`, not
   `0x8000`, so even an exact copy loses one LSB per entry; that off-by-one is
   how the probe tells which branch fired.
4. **Average**, `(a + b) / 2` through a 32-bit add-and-halve that stitches the
   sign bit back with `lsli/or`, clamped to `[0x8801, 0x77FF]`.

## Where the interpolated frame goes

Established, by reading `r0` at the synthesiser's entry rather than trusting the
decompiler — which had already been caught rendering
`Vocoder_DecodeAmbeFrame`'s marshalling in a way the machine contradicts.
`Vocoder_ConfigureFrame 0x00016CDC` renders as

```c
if (bSkipSignaling == 0) {
    Vocoder_ProcessFrameSignaling(..., pCtx + 1000, asStack_ac, pCtx);
    Vocoder_SynthesizeFrame(asStack_ac,            pOutBuf, nFrameSize, 0, pCtx);
} else {
    Vocoder_SynthesizeFrame((short *)(pCtx + 1000), pOutBuf, nFrameSize,
                            bSkipSignaling, pCtx);
}
```

and over 115 live calls that is exactly what the registers say:

```
Vocoder_SynthesizeFrame calls: 115
  handed the interpolated block (0x00057E14) 58
  handed PARAMS+0x000 (0x00045AA4)           57
  interpolated-then-current into adjacent 80-sample halves: 57
  other sources: 0
```

`r1` is the giveaway: the two destinations are `0x0004682A` and `0x000468CA`,
`0xA0` apart, which is 80 samples. So **each 160-sample AMBE frame is
synthesised as two 80-sample halves**, and the observed order never varies:

```
pend -> ConfigureFrame -> [interpolate] -> SYNTH(src=interpolated, dst=half 0)
pend -> ConfigureFrame ->                  SYNTH(src=PARAMS+0x000, dst=half 1)
```

Half 0 comes from the current and previous frames' envelopes resampled onto a
common pitch and averaged. Half 1 comes from the current frame alone. That also
explains the in-place log2 -> linear rewrite of `params+0x10` between the two
wakes: the second call is handed that block and normalises it where it sits,
which is why `.fwamps` carries linear amplitudes for the *second* half of the
frame while the first half's live only in the stack block.

## What this decoder does instead

`src/ambe_synth.c` synthesises all 160 samples in one pass, blending `prev` and
`cur` **per harmonic index `l`** through a trapezoidal window — mbelib's model,
and a defensible one. It is not the radio's. Two differences are structural:

* the radio blends **envelopes on a common frequency grid**, once, and applies
  the result to half the frame; this blends **harmonics by index**, continuously,
  across all of it;
* on the 53-in-232 frames that take the geometric-mean branch, the radio's first
  half uses a pitch and harmonic count belonging to **neither** coded frame. An
  index-matched blend cannot express that at all.

### What it would take, and what does not work

`src/ambe_blend.c` now transcribes the interpolation itself, bit-exact against
232 firmware calls (`tests/test_blend.c`). Wiring it in is the part that is not
a small change, and the reason is the window rather than the blend.

`ambe_synth.c`'s `ws_num` is a 321-entry trapezoid indexed by `n` and `n+N` —
the standard MBE overlap-add spanning two 160-sample frames. Making the hop 80
samples is not `N = 80`; it is a *different window*, which would be invented
here rather than transcribed. The radio does not use this mechanism at all: it
synthesises through an inverse FFT with `Vocoder_ApplySynthesisWindow
0x00029D1C`. So a faithful two-half implementation means transcribing
`Vocoder_SynthesizeFrame 0x00019DB8`, not adapting this one.

The cheap substitute was tried and **measured worse**: starting each frame from
the blend instead of from the previous frame drops the band correlation from
**0.973 to 0.962**, worst-case 0.805 to 0.638.  (Both baselines predate
`csky-mvcv.patch`, which moved the worst case to 0.831; the ablation has not
been re-run, and it is the *drop* that the argument rests on.) That is a result about the
substitution, not about the radio's model — the parameter sequence is
`[…, prev, mid, cur, mid', cur', …]` at 80-sample intervals, and feeding two of
those into a 160-sample-hop overlap-add stretches them over twice their
intended duration. There is no shortcut that preserves both the trajectory and
the frame-to-frame continuity; it needs the 80-sample-hop synthesiser.

So this is the honest account of the remaining synthesis distance — 0.973 band
correlation against the radio's own audio, versus mbelib's 0.968 on the same
reference. Closing it means synthesising in two halves from a resampled
parameter set, which is a change to the synthesis path, not to the decode.

**None of it moves the parity statement**, which is made against the coded model
parameters — classification, `L`, `f0`, voicing and the spectral envelope of
`PARAMS+0x000` — all exact or within 0.021 dB.

SPDX-License-Identifier: ISC
