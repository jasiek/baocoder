# The 1.15 dB amplitude gap

Everything above the spectral amplitudes is bit-exact against the firmware's own
decoder (`tests/test_firmware.c`). The amplitudes are not. This is how far the
chase has got, written down so the next attempt starts where this one stopped.

## The method that made it tractable

The parked array at `params+0x10` is linear int16, and comparing against it
mixes the envelope decode with the log2->linear conversion and the enhancement.
Stub **both** of the stages that come after the decode and the block keeps the
raw log2 envelope, which can be compared against `ambe_parms.log2Ml` harmonic by
harmonic:

```sh
python3 tools/fw_oracle/gen_jobs.py job.txt frames.txt 60   # + hook_ret on
$REVENG/tools/emu/run.sh job.txt out.txt                    #   0x00022C18 and
                                                            #   0x000277F8
```

Feed one voice payload repeated ~24 times and the envelope predictor reaches its
fixed point, so what is left is the per-frame decode with no transient in it.

## What that shows

At the fixed point, on `L = 48`, `firmware - baocoder` in log2 units:

```
 +1.97  -0.00 -0.00 +0.01 -0.01 +0.01 -0.00 -0.00 +0.01 -0.01     harmonics 1-10
 +0.00 +0.00 -0.03 -0.00 -0.01 +0.00 +0.01 +0.00 -0.01 +0.00      11-20
 +0.00 -0.01 -0.01 -0.02 -0.00 -0.01 +0.00 -0.00 +0.01 -0.01      21-30
 +0.00 +0.03 -0.05 -0.19                                          31-34
 -0.29 -0.41 -0.57 -0.80 -1.10 -1.41 -1.74 -1.92 -1.83 -1.51      35-44
 -1.09 -0.82 -0.73 -0.78                                          45-48
```

**Harmonics 2 to 34 agree to +-0.03 log2 — 0.06 dB.** Projected onto the codec's
own DCT basis per prediction block (`Ji = [10, 11, 13, 14]` for `L = 48`):

| block | harmonics | k=1 | k=2 | k=3 | k=4 |
|---|---|--:|--:|--:|--:|
| 1 | 1..10 | +0.39 | +0.39 | +0.37 | +0.35 |
| 2 | 11..21 | −0.01 | −0.00 | −0.00 | +0.01 |
| 3 | 22..34 | −0.04 | +0.03 | −0.04 | +0.03 |
| 4 | 35..48 | **−2.14** | **+0.23** | **+0.67** | −0.06 |

Block 1's flat row is the signature of one spiked sample, not a coefficient
error: a delta at `j = 0` projects onto every `k` at nearly the same size, and
`2 x 1.97 / 10 = 0.394` is that size. So:

**The divergence is exactly two things — harmonic 1, and prediction block 4.**
Blocks 2 and 3 are zero on every coefficient. In the codec's `a0 = 1`, `ak = 2`
convention block 4's are `dC1 = −1.07`, `dC2 = +0.11`, `dC3 = +0.33` — its DC,
its tilt *and* its first higher-order coefficient, which come from two different
codebooks (`Ri[7]`/`Ri[8]` via PRBA24+PRBA58, and the b8 HOC). Working the DC and
tilt back gives `dRi[7] = −0.91`, `dRi[8] = −1.23`, while `Ri[1..6]` — the same
eight-point DCT of the same `Gm` — are exact.

## The firmware's chain, read

Ghidra's plugin was up, so the whole path was read rather than inferred.

| | |
|---|---|
| `Vocoder_CodeSpectralEnvelope 0x000226F4` | two-call wrapper: gain, then coefficients |
| `Vocoder_CodeSpectralCoefficients 0x000220D4` | the envelope codec, both directions |
| `Dsp_ResampleInterpolate 0x0002685C` | the DCT — a phase accumulator over `g_awSineTable512`, `a0 = 1`, `ak = 2`, stepping by `floor(32768/n)` from a table at SRAM `0x18003840` |
| `Dsp_HilbertTransform 0x00029D1C` | **reads** the log2 envelope into `pChannelState+0x172`; does not modify it. It extrapolates past `L` at −0.72 log2 per harmonic |
| `Vocoder_NormalizeSpectralBlock 0x00022C18` | log2 -> linear: clamps to `[0x8800, 0x77ff]` (+-15 at Q11), takes the block exponent as `floor(max) + 1`, then `Math_Pow2Scaled` per harmonic, exponent out at `params+0x84`, tail zero-filled to 56 |
| `Math_Pow2Scaled 0x00019280` | the same six-coefficient Taylor core as `Math_Pow2`, coefficients at SRAM `0x1800160C` — the ones `ambe_basop.c` already uses — with a caller-chosen output Q |
| `Vocoder_MatchExcitationEnergy 0x000277F8` | the enhancement. Voice frames only, and it takes `f0`, `L` **and two words of persistent channel state** at `0x7c0`/`0x7c2` — so it is stateful, where `ambe_enhance_spectrum` is not |

`Vocoder_SynthesizeFrame 0x00019DB8` runs them in that order: Hilbert, then
normalise, then enhance.

Every step of the envelope decode was checked against `src/ambe_params.c` and
agrees: the Q16 `prevL/curL` prediction step, the interpolation between
harmonics `idx` and `idx+1`, the `idx == 0` case, the pad-to-56 tail, the 0.65
damping with its mean subtracted, `Gm[1] = 0`, the `(a+b)/2` and
`(a-b)/(2*sqrt(2))` rotation, `min(Ji-2, 4)` HOC coefficients, the
six-coefficient block DCT, and `BigGamma = gamma - 0.5*log2(L) - mean(Tl)`.

The seven codebook pointers in the literal pool at `0x0002247C..0x00022494` were
read out of the live program and are **exactly** the addresses
`tools/extract_tables.py` extracts from, and the block-length table matches the
image byte for byte on all 48 rows.

## What it is not

Each measured, not argued:

* **Not the enhancement.** Ablate `Vocoder_MatchExcitationEnergy` from the
  firmware and `ambe_enhance_spectrum` from here, and the residual is the same
  shape at the same size (1.25 dB against 1.10). The two agree.
* **Not its parameters** — 1.2/0.5 clamps optimal, `8l <= L` octave exemption
  optimal, renormalisation irrelevant.
* **Not the resample ratio** (`prevL/curL` right, the `f0` ratio costs 0.11 dB),
  **not the `Ri -> Cik` rotation** (`1/(2*sqrt(2))` right, `1/sqrt(2)` costs
  4.2 dB), **not the codebook addresses**, **not the block lengths**.
* **Not the predictor** — the divergence is at full size at the fixed point and
  on the first voice frame of a capture alike.
* **Not the fixture's int16 precision** — flat with depth below the frame's peak
  until the last few bits.

## Where to look next

Two leads, both narrow:

1. **Block 4.** Its DC, tilt and first HOC coefficient are all wrong while
   blocks 1..3 are exact, and the machinery is shared — the same `Gm`, the same
   eight-point DCT, the same rotation, the same six-coefficient block DCT. Some
   thing is different about the *last* block specifically. Reading
   `Vocoder_CodebookVectorLookup 0x0002369C` is the obvious next step, since it
   is what writes both the PRBA vectors and the HOC ones and takes a per-call
   length that is `4` for the HOC and `3`/`4` for the PRBA.
2. **Harmonic 1**, off by up to +-2 log2 and drifting frame to frame. It is the
   one harmonic whose prediction takes the `idx == 0` path, which both
   implementations appear to handle the same way.

Everything needed to continue is in place: `tools/fw_oracle/` regenerates the
fixtures, the two-stub trick above exposes the raw log2 envelope, and Ghidra's
plugin answers `decompile_function`, `get_xrefs_to` and `read_memory` over HTTP
on port 8089.

SPDX-License-Identifier: ISC
