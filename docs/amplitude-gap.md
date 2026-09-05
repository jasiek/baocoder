# The amplitude gap — resolved, and what replaced it

The "1.15 dB spectral amplitude gap" was a **comparison error, not a decoder
error**. The fixture was reading the wrong parameter block.

## What it actually is

`tests/fixtures/*.fwamps` was exported from the block at `params+0x10`. That is
the array `Vocoder_SynthesizeFrame 0x00019DB8` synthesises from — and by the time
it is written, `Vocoder_ResampleSpectralEnvelope 0x00026A84` has replaced it with
an **interpolation of this frame's envelope and the previous frame's** onto a
common pitch. This decoder has no counterpart for that stage, so comparing
`log2Ml` against it charged the envelope decode 1.15 dB for something downstream
of it.

The frame's *own* decoded envelope is in the **fourth** parameter block, at
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

## What is left

**`Vocoder_ResampleSpectralEnvelope 0x00026A84` is not implemented here**, and
it is the only thing that stands between this decoder and the block the radio
synthesises from. It is now read in full, along with both of its helpers:

* **`Vocoder_ComputeHarmonicResampleRatio 0x000269B0`** builds a 60-entry window
  from a 56-entry envelope — `buf[0] = src[0]`, `buf[k] = src[k-1]` for
  `k = 1..56`, three edge-holds above — so index `k` *is* harmonic `k` and
  harmonic 0 holds harmonic 1. It takes `Math_DivideNormalized 0x0002692C` of
  the two pitches (Q16, short-circuiting to `0x10000` when they are equal), then
  accumulates that ratio once per output harmonic and linearly interpolates:
  **`out[l] = src at harmonic position l * f0_out / f0_src`**. That is
  frequency-matched resampling, and it is a different law from the envelope
  predictor's `prevL/curL` index ratio in `src/ambe_params.c`.
* **`FUN_00022024 0x00022024`** is a pure copy — class, `L`, `f0`, the voicing
  word and the `L`-entry envelope from the frame's params into the prediction
  state, padded to 56 with the last value. That is why the fourth block holds
  the current frame's envelope by the time the emulator peeks it.

So the algorithm reads as: pick a pitch (the current `f0`, the previous, or
`sqrt(2*f0cur*f0prev)`, on the two voicing words against the masks at
`0x00026C64`/`0x00026C68`), take `Vocoder_HarmonicCountFromPitch` of it, resample
both frames' envelopes onto that grid, and average.

**Transcribing that does not reproduce the block.** With the resampler written
out exactly as above and the pitch resolving to the current frame's — which it
does, since the block's `L` and `f0` match this decoder's on all 2052 frames —
the average scores 3.65 dB against the block, *worse* than not interpolating at
all (3.34 dB). A per-frame least-squares fit of `a*current + b*previous` returns
**`a = 1.02`, `b = 0.075`** with the residual still at 3.3 dB: no linear
combination of the two envelopes explains it. Blocks 0 and 1 are byte-identical
on every frame, so they are a copy pair, not a raw/interpolated pair.

Something else is therefore happening between the envelope and that block, and
the four functions the decode path calls after `Vocoder_CodeSpectralEnvelope`
have all now been read and none of them accounts for it — `FUN_00029914` is
gated off in this configuration (ablating it changes nothing), `FUN_00022024` is
a copy, and the resampler is the law above.

**This does not affect the parity statement.** The block in question is a
synthesis-side intermediate; every *model parameter* — classification, `L`,
`f0`, voicing and the spectral envelope — is exact or within 0.021 dB, and the
synthesised audio correlates with the radio's own at 0.973 against mbelib's
0.968 on the same reference.

SPDX-License-Identifier: ISC
