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

## What is left

**Where the interpolated frame goes is not established.** It is
`Vocoder_DecodeAmbeFrame 0x0002033C`'s `pOutFrame`, but that function's
rendering in `Vocoder_ProcessFrameSignaling 0x000198CC` —
`Vocoder_DecodeAmbeFrame(pFrameParams, 0x44, pOutFrameParams, 0x44, ...)` —
does not match the register arguments the emulator observes, so the decompiler
cannot be trusted on the marshalling here and the claim is not made. Reading it
means breaking further up the call chain, which is the obvious next step.

If it is what the synthesiser consumes, then on the 53-in-232 frames that take
the geometric-mean branch the radio synthesises at a pitch and harmonic count
belonging to neither coded frame, and this decoder does not model that.

**None of this moves the parity statement**, which is stated against the coded
model parameters — classification, `L`, `f0`, voicing and the spectral envelope
of `PARAMS+0x000`, all exact or within 0.021 dB — and the synthesised audio
correlates with the radio's own at 0.973 against mbelib's 0.968 on the same
reference.

SPDX-License-Identifier: ISC
