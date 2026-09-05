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

**`Vocoder_ResampleSpectralEnvelope 0x00026A84` is not implemented here.** It
picks a pitch — the current frame's `f0`, the previous frame's, or
`sqrt(2*f0_cur*f0_prev)`, chosen on the two frames' voicing words against the
masks at `0x00026C64`/`0x00026C68` — takes `Vocoder_HarmonicCountFromPitch` of
it, resamples both frames' envelopes onto that grid with
`Vocoder_ComputeHarmonicResampleRatio 0x000269B0`, and averages them, clamping to
`[0x8801, 0x77ff]` and padding to 56.

Its `L` and `f0` match this decoder's on all 2052 corpus frames, so the pitch
selection resolves to the current frame's there. Its envelope does not: the
straightforward reading — average the current envelope with the previous one
resampled by the `f0` ratio — scores 3.66 dB, worse than not interpolating at
all (3.15 dB), so `Vocoder_ComputeHarmonicResampleRatio` does something other
than the obvious and is the next function to read.

Whether this decoder *should* implement it is a separate question: the radio
synthesises two 10 ms halves and this is how it builds the envelope for one of
them, where `ambe_synth.c` interpolates in the overlap-add instead. The audio
comparison is unaffected — 0.973 band correlation against the radio's own PCM,
against mbelib's 0.968 on the same reference.

SPDX-License-Identifier: ISC
