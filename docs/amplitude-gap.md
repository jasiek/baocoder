# The 1.15 dB amplitude gap

Everything above the spectral amplitudes is bit-exact against the firmware's own
decoder (`tests/test_firmware.c`). The amplitudes are not, and this is what the
chase established. It is written down because the next person to pick it up
should not repeat the eliminations.

## What the residual is

Per frame, the RMS of `log2(firmware) - log2(ours)` after removing the
block-float offset, over 1 662 voice frames of the six captures:

| | |
|---|---|
| median | **0.191 log2 = 1.15 dB** |
| correlation, log domain | 0.997 |

It is not noise. It is a **fixed spectral tilt**, the same on every frame:

| l/L | 0.0 | 0.2 | 0.4 | 0.6 | 0.7 | 0.8 | 0.9 | 1.0 |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| median residual, log2 | −0.09 | −0.10 | −0.10 | −0.10 | −0.02 | +0.16 | +0.32 | +0.46 |

Flat below about `0.6 L`, then a clean linear ramp to **+0.46 log2 (2.8 dB)** at
the top harmonic. The ramp's length scales with `L` — it is the top 40% of the
harmonics, not a fixed count — and in frequency it starts around 2.2 kHz and
reaches +3.5 dB at 3.7 kHz.

**It is at full magnitude on the very first voice frame of a capture** (0.26
mean, against 0.19 settled) and does not build up, so it is not the envelope
predictor accumulating anything. It is a per-frame error.

Projected onto the codec's own DCT basis, block by block, all of it lands in the
**last prediction block's k=1 and k=2** — its DC and its tilt. Blocks 1..3 and
every coefficient k >= 3 project to zero.

## What it is not

Each of these was measured, not reasoned about:

* **Not the spectral enhancement.** `Vocoder_MatchExcitationEnergy 0x000277F8`
  is the firmware's enhancement stage — found by ablation, stubbing it moves the
  spectrum by −0.59 log2 at the bottom and +0.48 at the top. Stub it *and* drop
  `ambe_enhance_spectrum` from this decoder, and the residual is **the same
  shape and the same magnitude** (1.25 dB). So the two enhancements agree and
  the gap is upstream of both.
* **Not the enhancement's parameters** — the 1.2/0.5 clamps are optimal (1.5
  gives 1.23 dB, 2.0 gives 1.28), the lowest-octave exemption is optimal at
  `8l <= L` (6 gives 1.27 dB, 4 gives 1.73), and removing the energy
  renormalisation changes nothing.
* **Not the prediction resample ratio.** `prevL/curL` is right; resampling by
  the `f0` ratio instead costs 1.16 -> 1.27 dB.
* **Not the `Ri -> Cik` rotation.** `1/(2*sqrt(2))` is right: doubling it to
  `1/sqrt(2)` costs 1.16 -> 5.4 dB, and `0.5` costs 2.6 dB.
* **Not the b3/b4 bit assignment** — that was a separate finding, already
  applied, and worth 2.58 -> 1.44 dB on its own.
* **Not the predictor rule** — also separate, also applied.
* **Not the fixture's precision.** The residual is flat with depth below the
  frame's peak until the last few bits, where the firmware's int16 runs out.

## The firmware's envelope decoder, read

`Vocoder_CodeSpectralEnvelope 0x000226F4` is a two-call wrapper;
`Vocoder_CodeSpectralCoefficients 0x000220D4` is the whole envelope codec, both
directions. Read end to end against `src/ambe_params.c`, **every step agrees**:

| step | firmware | this decoder |
|---|---|---|
| prediction step | `Math_SDivHalf(0x80000, L)`, Q16 `prevL/curL` | same |
| interpolation | between harmonics `idx` and `idx+1`, `idx = step>>16` | same |
| `idx == 0` | takes harmonic 1 unchanged | same, via `log2Ml[0] = log2Ml[1]` |
| tail | pads the array to 56 with the last harmonic | same |
| damping | `* 0xa666 >> 1` = 0.65, with `0.65 * mean` subtracted | same |
| `Ri` | 8-point DCT of `Gm[1..8]`, `Gm[1] = 0` | same |
| `Ri -> Cik` | `C1 = (a+b)/2`, `C2 = (a-b) * 0xb504 >> 16` after a `>>1` — `1/(2*sqrt(2))` | same |
| HOC | `min(Ji-2, 4)` coefficients at `k = 3..6` | same |
| block IDCT | 6 coefficients, `a0 = 1`, `ak = 2` | same |
| DC | `gamma - 0.5*log2(L) - mean(Tl)` | same |
| sum | `Tl + BigGamma + 0.65*(P - mean P)` | same |

Two facts worth having, neither of which resolves the gap:

* The DCT is **`Dsp_ResampleInterpolate 0x0002685C`** — a phase accumulator over
  the same 512-entry cosine table the synthesiser uses (`g_awSineTable512`,
  SRAM `0x18001630`), stepping by `floor(32768/n)` from a table at SRAM
  `0x18003840`. The truncation in that step is at most 3.7e-4 relative, which is
  three orders of magnitude too small to be this.
* The parameter block's amplitude array is written by the envelope decoder in
  the **log2 domain**, clamped to `[0x8800, 0x77ff]` — +-15 at Q11 — and
  converted to the linear int16 that `tests/fixtures/*.fwamps` carries later,
  by `Math_Pow2Scaled 0x00019280`.

## Where to look next

The envelope decode agrees, the enhancement agrees, and the gap is a fixed tilt
in the top 40% of the spectrum that is fully present on the first frame. That
leaves the two stages between them, neither of which has been read:

1. **`Math_Pow2Scaled 0x00019280` and the block-float packing** — the log2 ->
   linear conversion and the shared-exponent normalisation. A per-harmonic
   exponent difference here would show up exactly as a tilt, because the top
   harmonics are the quiet ones.
2. **`Vocoder_LookupAndBlendGain 0x0002AE70`** — it produces the gain the
   envelope decoder is handed. A pure DC term cannot make a tilt, but it has not
   been read and its second argument is `pFrameParams + 8`, the voicing word,
   which is not obviously a gain input.

The instruments are all in place: `tools/fw_oracle/` regenerates the fixtures,
and the ablation method (stub a function with `hook_ret`, diff the parked array)
localises a stage in one 40-second run.

SPDX-License-Identifier: ISC
