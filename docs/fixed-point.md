# Fixed point, and what it cost

The radio has no FPU. `docs/FINDINGS.md:186-194` in `baofeng-dm32uv-reveng`
establishes that the DM-32UV's toolchain was configured soft-float and that the
vocoder does not even call that: its DSP path runs ITU-T G.191 style basic
operators over a hand-rolled block-floating-point package in IRAM, whose fifty
callers are the vocoder. `FINDINGS.md:359` identifies the layer beneath as the
basic operators themselves — `Math_Log2` `0x0001903C`, `Math_Pow2` `0x000191C0`,
`Math_Sqrt` `0x00019364`, `Math_SDiv` `0x00018D74`.

So the codec this project reimplements is integer-only, and this branch makes
this one integer-only too. `make check-fixedpoint` asserts it rather than
claiming it: `libbaocoder.a` references no libm symbol, links with no `-lm`, and
contains **zero floating-point instructions**.

The question that motivated the branch was what that costs in precision. The
short answer is that it costs a factor of thirty in the spectral envelope,
which is still well inside the codec's own quantiser step, and that it *gains*
five decibels in the synthesised audio.

## The primitives come out of the image

The coefficients are bytes lifted from the firmware, not a reconstruction. Each
primitive's literal pool points into one contiguous SRAM block:

| Primitive | pool at | SRAM | file | contents |
|---|---|---|---|---|
| `Math_Log2` `0x0001903C` | `0x000190BC` | `0x18001600` | `0x64CC0` | 6 × int16 Q15 |
| `Math_Pow2` `0x000191C0` | `0x0001927C` | `0x1800160C` | `0x64CCC` | 6 × int16 Q15 |
| `Math_Sqrt` `0x00019364` | `0x000193DC` | `0x18001618` | `0x64CD8` | 12 × int16 Q15 |
| `Math_TableInterpLookup` `0x00019000` | `0x00019038` | `0x18001630` | `0x64CF0` | 512 × int16 |

`0x18001600`–`0x1800182F` is perfectly contiguous, which is the same structural
confirmation the codebook identification rested on. The values identify
themselves: log2's leading coefficient is 1/√2, pow2's fifth is `0x58B9` = ln2,
and the 512-entry table is **cosine** — it matches `32767·cos(2πi/512)` to within
one LSB at every point, despite the `g_awSineTable512` name it carries in the
reverse-engineering ledger.

The IRAM window that holds the code is in the image too. `TARGET.md` records
IRAM `0x00010000–0x0002F097` as copied from flash `0x030BD768`, which with the
existing SRAM anchor gives `file = VA − 0x10000 + 0xB1868`. At the computed
offset for `Dsp_LcgSignScramble` `0x00021FE0` the bytes are `… ad 35 … 19 36 …`
— the `0xAD`/`0x3619` LCG constants, in C-SKY encodings.

### How accurate the radio's own maths is

`tests/test_basop.c` sweeps each primitive across its whole input domain against
libm. These are the radio's accuracies, not this implementation's:

| primitive | worst error | why |
|---|---|---|
| `cos` | 6.10e-05 abs | 512-entry table, linear interpolation — exactly `(2π/512)²/8` |
| `log2` | 2.65e-03 abs | four-term Taylor about 1/√2, **not** a minimax fit |
| `pow2` | 1.16e-04 rel | five-deep Horner |
| `sqrt` | 1.29e-03 rel | two-deep Horner plus a √2 correction chosen by shift parity |
| `pow2(log2(x))` | 1.92e-03 rel | the two composed |

log2 being a Taylor series rather than a minimax fit is worth stating, because
it is much less accurate than a 6-coefficient polynomial has to be: its error is
~1e-6 near the centre of `[0.5, 1)` and 2.6e-3 at the ends. That is why
`0.5·log2(L)` is a 57-entry constant table in `ambe_params.c` rather than a call
to `ambe_log2` — L takes only 48 values, so the table is exact, and that term
lands directly on every spectral amplitude in the frame.

The four log2 coefficients match the Taylor coefficients of log2 about 1/√2 —
2.0402, −1.4427, 1.3603, −1.4427 — to five digits. That pins the polynomial's
shape independently of the numerical agreement, and is asserted in the test.

### Two decompiler defects, both caught by measurement

Neither was visible by reading. Both produce plausible C that computes something
else.

* `Math_TableInterpLookup`'s return decompiles as `uint`, so the final shift
  reads as logical. It is arithmetic — the table is negative over half its
  range, and with the wrong shift every cosine in the lower half-turn comes back
  as a large positive number.
* `Math_Pow2`'s fraction extract is `zext r2,r0,0xf,0x1` at `0x000191C6`, a
  bitfield extract of bits [15:1]. Ghidra renders it as `(v & 0x7fff) >> 1`,
  which masks before shifting and drops the fraction's top bit. With that
  reading, `pow2` ignores its fractional argument entirely and returns powers of
  two. The disassembly settles it.

The lesson is the cheap one: sweeping a transcribed primitive across its whole
domain against a reference costs a few lines and catches what review does not.

## What the numbers are measured against

Comparing this branch against the float library on `main` says how far apart
they are, not which is closer to the exact result. `tools/make_oracle.py`
generates the reference: the float implementation's own source, retyped to
double — same algorithm, same constants, same order of operations, ~15
significant digits instead of ~7. Generating it rather than writing one by hand
is the point; a hand-written reference is a second implementation, and a
disagreement would not say which side had the bug.

It emits **two** oracles, and the reason is load-bearing. The fundamental feeds
a phase accumulator that runs the length of the transmission, so a 4e-4 relative
difference in `f0` becomes a growing time shift in the audio — tens of dB of
apparent error that says nothing about how the arithmetic is carried. Comparing
against the exact-`pow` oracle measures the pitch law; comparing against the
firmware-pitch oracle measures the arithmetic. Reporting only the first would
blame fixed point for a difference in the pitch law. Reporting only the second
would hide that difference.

## Results

2052 real DM-32 frames across six captures, worst case over every frame and
every harmonic. Each implementation is measured against its matched oracle.

### Model parameters

| | ΔL | ΔV | w0 | γ | log2Ml | Ml |
|---|---|---|---|---|---|---|
| float vs oracle | 0 | 0 | 1.36e-08 | 1.54e-07 | 1.28e-05 | 2.60e-05 |
| **fixed** vs oracle (firmware pitch) | 0 | 0 | 3.48e-06 | 5.94e-08 | 3.73e-04 | 5.86e-03 |

The harmonic count and every voicing decision are exact in both. γ is slightly
*better* in fixed point, because the gain accumulator is a Q24 integer where the
float version repeatedly halves a float.

`log2Ml` is the real cost: 3.73e-04 against float's 1.28e-05, a factor of
thirty. It is worth putting that next to the quantiser it feeds — the codec's
own tables are Q11, one step of which is 4.9e-04. The fixed-point envelope error
is smaller than the step size of the thing being represented.

### Synthesised audio

Sample by sample over all 2052 frames. This is only legitimate because the noise
generator is integer on both sides and its state update is identical, so the
same draws come out in the same order as long as L and the voicing decisions
agree. The harness checks rather than assumes: **0 of 2052 frames** differ.

| | SNR | worst frame |
|---|---|---|
| float vs oracle | 54.1 dB | 30.0 dB |
| **fixed** vs oracle (firmware pitch) | **59.5 dB** | 34.9 dB |
| the pitch law alone | 4.7 dB | −5.8 dB |

Fixed point is five decibels *ahead* of float here, and the reasons are
structural rather than lucky:

* **The phase accumulator is exact.** `PSIl` accumulates every frame without
  bound, so in a float it loses mantissa bits as a transmission goes on — after
  a few hundred frames it is tens of thousands of radians and quantised
  accordingly. Here it is a wrapping `uint32` in Q32 turns, which is exact
  modulo one turn for ever.
* **The window is exact.** Every tap of the overlap-add trapezoid is k/50 for an
  integer k, and both taps at a given sample carry the same 1/50, so the divisor
  factors out of the harmonic sum entirely. It is applied once at the end with
  the codec's ×7 output gain, as a single ×7/50. The window contributes no
  rounding error at all, where a Q15 copy of it would have.

The third row is the one to keep in view: the difference between the radio's 2^x
polynomial and an exact `pow()` is worth 4.7 dB on its own, which is far more
than the arithmetic. Any audio comparison that does not control for the pitch
law is measuring the pitch law.

### Against mbelib and JMBE

mbelib is a third independent implementation, not ground truth: it is float, and
it reconstructs the codebooks itself where this decoder reads the radio's Q11
tables, so a half-LSB disagreement sits under every figure.

| | ΔL | w0 | γ | log2Ml |
|---|---|---|---|---|
| float | 0 | 3.93e-04 | 2.69e-04 | 6.63e-03 |
| **fixed** | 0 | **3.14e-06** | 2.69e-04 | 6.71e-03 |

Against JMBE's `expected.wav` over all six captures, mean band-energy
correlation is 0.965 for float and 0.966 for fixed.

## The pitch law, and a correction to the README

`ambe_pitch_from_b0` now evaluates the radio's own polynomial
(`Vocoder_PitchFromLog2` `0x0002AD6C`) instead of `pow(2, x/4096)`. That closes
the gap to mbelib rather than widening it: `f0` agrees to **4.8e-05** relative
where an exact `pow()` left 2.4e-03, and all 120 harmonic counts match where the
exact version disagreed at b0 = 17.

This contradicts what `README.md` currently says. Its account is that mbelib's
tabulated f0 sits above the L = 11/12 boundary, the firmware's law lands below
it, and "the firmware is the shipping implementation and is taken as correct."
The premise was wrong: that divergence came from evaluating the firmware's law
with an exact `pow()`, which the firmware does not have. With the firmware's own
four-term Taylor polynomial there is no divergence at all.

The stronger reading is that this is evidence rather than a tolerance. b0 = 17
sits on an L boundary; an exact computation lands one side of it and the radio's
approximation the other; and mbelib's independently-derived table agrees with
the radio. Two implementations that share an approximation agreeing on a
boundary case that an exact computation gets wrong says the approximation is
what the codec is specified in terms of — not an artefact of this radio.

`tests/test_tables.c` now asserts zero L divergences and `f0` within 1e-4. Both
are tighter than what they replaced.

## Where the number formats came from

Measured over the corpus, not guessed:

| quantity | range over 2052 frames | format |
|---|---|---|
| γ, log2Ml | [−7.6, 11.5] | Q24 — ±128 range, ten times finer than float there |
| Ml | [0.0046, 2013.6], 4.4e5 dynamic | block float, one exponent per frame |
| f0 | the firmware's own clamp | Q19 turns/sample, the scale the radio emits |
| phase | unbounded in float | wrapping uint32, Q32 turns |

The amplitudes are block floating point because no single Q format covers 4.4e5
usefully. That is also how the radio carries spectral data
(`Vocoder_NormalizeSpectralBlock` `0x00022C18` normalises an array to a common
exponent).

One deliberate departure: the radio's spectral arrays are int16 and these
mantissas are int32. The reason is measured — within a single frame the
loudest-to-quietest amplitude spread reaches **13.5 bits**, so a 15-bit mantissa
would leave the quietest harmonic in a frame under two bits.

## What got better, what got worse

Better:

* Synthesised audio, 54.1 → 59.5 dB against matched references
* `w0` against mbelib, 3.93e-04 → 3.14e-06
* Harmonic count against mbelib, one divergence → none
* γ, 1.54e-07 → 5.94e-08
* The codebook searches are now exact integer distances in Q11. The float
  version divided all 2368 table entries by 2048 and compared in floating point,
  adding a rounding step per candidate; the encoder's bit-exact rate is
  unchanged at 1354/1664, so this bought correctness of reasoning rather than
  measurable accuracy
* Analyser pitch, 172/191 → 173/191 within four quantiser steps; level ×0.91 → ×0.93
* JMBE band correlation 0.965 → 0.966

Worse:

* `log2Ml`, 1.28e-05 → 3.73e-04 — a factor of thirty, still inside the Q11
  quantiser step of 4.9e-04 that the codec applies to it anyway
* `Ml`, 2.60e-05 → 5.86e-03, the same effect after the exponential
* Every transcendental now carries the radio's accuracy rather than libm's, and
  `log2` in particular is only good to 2.6e-3

Unchanged: the harmonic count, every voicing decision, the frame
classification, the FEC, and the encoder's 1354/1664 bit-exact rate.

## Test tolerances

One assertion changed, and it tightened: `tests/test_tables.c` went from
"exactly one L divergence from mbelib" to "none", and its `f0` bound from 3e-3
to 1e-4. No tolerance was widened.

`tests/test_basop.c` is new — 10 621 checks sweeping the transcribed primitives
against libm across their whole domains.

## Transcribing the radio's analyser

Attempted after the rest landed. Real ground was gained and a hard boundary was
reached; both are recorded here because the boundary is the useful part.

### The analysis cluster is the encoder front end

`docs/FINDINGS.md` in the reverse-engineering project notes that the
FFT-windowing pipeline is called only from `Vocoder_RxTask` and never from
`Vocoder_TxTask`, which reads as though it belongs to the receive path. It does
not. `Vocoder_ProcessFrame` `0x00016E04` takes a PCM pointer, scales it through
`Vocoder_ScaleSamplesForAnalysis` `0x00018BCC`, shifts a three-deep rolling
window of 258 samples, windows it into an FFT, and runs a spectral analysis and
a candidate search over the result. A decoder has no use for a rolling window of
input samples. The "Rx" in `Vocoder_RxTask` is about receiving *audio*.

The call graph is genuinely split across the two tasks — `Vocoder_SynthesizeFrame`
`0x00019DB8`, which is decode, hangs off `Vocoder_TxTask` — so the task names
carry no reliable direction information either way. The code does.

### What was transcribed

The analysis window and its geometry. `Vocoder_ProcessFrame` hands
`Dsp_WindowAndComputeFft` `0x00019B6C` a table from its literal pool at
`0x00016F38`, which reads SRAM `0x180010A8` (file `0x064768`), with
`nInLen = 199` and a 256-point transform. The stock code stores half the window
and folds it symmetrically, so the table is 100 entries. Scaled by its own peak
of 29883 it reproduces `0.54 − 0.46·cos(2πi/198)` at every one of those 100
points to within one count: a **199-point Hamming window**.

It is extracted by `tools/extract_tables.py`, asserted against the Hamming
definition in `tests/test_tables.c`, and used by `ambe_analysis.c` — 199 windowed
samples zero-padded to 256, which is the radio's arrangement, in place of the
256-point window this code generated for itself. The measured effect on
`test_encode_pcm` is that the analyser's level accuracy improves from ×0.93 to
**×0.99**, with pitch unchanged at 173/191 within four quantiser steps.

### The module was broken, not the firmware

The FFT's inner kernels would not decompile — `Dsp_FftStageButterfly`
`0x00025160` and `Dsp_FftFinalStageButterfly` `0x0002509C` both emitted
`halt_baddata()` at their first line — and the cause turned out to be a gap in
the Ghidra processor module rather than anything about the image.

`taligentx/ghidra_csky_ck804` v0.2 implements the unsigned `mulu`/`mulua`/`mulus`
family (32×32 into the `hi:lo` accumulator) and the plain `mulsh`, but not the
signed 32×32 family nor `mulsh`'s accumulating variants:

```
i32_r_sop = 0b100011  pcode 1/2/4  ->  muls / mulsa / mulss
i32_r_sop = 0b100100  pcode 2/4    ->  mulsha / mulshs
```

Ghidra's flow analysis stops dead at an instruction it cannot decode, so every
function containing one is silently truncated. The encodings were identified by
their neighbours rather than from a manual — each has the shape of the unsigned
family one sop lower, and the C-SKY V2 multiply group pairs unsigned and signed
families adjacently — and a table learned from 884 register-ALU instructions
that Ghidra *does* decode confirmed none of the five appeared in the implemented
set.

Confirmed by result, not by the reasoning. On bytes Ghidra had never reached,
the first family decodes as `muls r0,r2 / mfhi r12 / mflo r0` — the accumulator
read that has to follow it. `docs/patches/csky-muls-family.patch` carries the
fix, in the same form as the reverse-engineering project's existing
`csky-movih-truncation.patch`.

One trap worth recording: a corrected sleigh does not by itself fix a program
Ghidra has already failed on. It records an error at the address and will not
retry, so `ClearAndRedisasm.java` and `RepairTruncated.java` beside the patch
clear the stale code units and re-disassemble.

What it recovered:

| function | before | after |
|---|---|---|
| `Dsp_FftStageButterfly` `0x00025160` | 68 bytes, truncated | 190 bytes, **0 warnings** |
| `Dsp_FftFinalStageButterfly` `0x0002509C` | 68 bytes, truncated | 190 bytes, **0 warnings** |
| `Dsp_FftButterflyRecurse` `0x00025484` | 40 bytes, truncated | 150 bytes, **0 warnings** |
| `Vocoder_AnalyzeSpectrum` `0x000205B8` | 393 lines, 10 warnings | 746 lines, **0 warnings** |
| `Vocoder_SynthesizeVoiced` `0x0001DE10` | 366 bytes, 9 warnings | 2378 bytes, **0 warnings** |

`Vocoder_AnalyzeSpectrum` is the speech analyser's main body. It had been
decompiling as a third of itself.

### What the transform turns out to be

With the kernels readable, the butterfly is textbook:

```
a = X + W·Y,   b = X − W·Y
Re(W·Y) = wr·yr − wi·yi,   Im(W·Y) = wi·yr + wr·yi
```

in Q15, the intermediate stage shifting `>>15` and the final stage `>>16` — the
block-float step `Dsp_FftBitReverseScale`'s exponent bookkeeping accounts for.
The whole chain is a 128-point *complex* FFT over 256 real samples packed
even/odd into re/im, with `Dsp_FftButterflyStage` doing the real-spectrum
unpacking afterwards. Its three tables are now extracted and asserted against
what they must reproduce (`tests/test_tables.c`): 256 twiddles of
`exp(−j2πk/512)` to 1 LSB, and two delta-coded permutation tables that decode to
the bit-reversal of N=32 and N=128 exactly.

### What remains

The transform is now fully readable but not yet written in C. In order:

1. Transcribe the FFT into `src/ambe_fft.c` and check it against a reference
   DFT, the way `tests/test_basop.c` checks the primitives. This is where a
   wrong transcription would first become visible, so it comes before anything
   that consumes the spectrum.
2. Settle the role of `Vocoder_SelectVoicingCandidate` `0x000177F0` — the
   evidence says pitch, not voicing — and transcribe
   `Vocoder_UpdateEnergyAndHistory` `0x000217CC`, which builds the array it
   searches.
3. Then `Vocoder_AnalyzeSpectrum` `0x000205B8` itself, and the pitch and
   voicing decisions. Its callee `Vocoder_NormalizeSpectralArrays` `0x0002A70C`
   still truncates at 28 lines — a different blocker, not this one.

## Still open

Beyond the analyser: nothing. Every table the decoder reads and every
transcendental it evaluates now comes out of the firmware image.

## Reproducing

```
make && make test              # includes check-fixedpoint
python3 tools/compare_precision.py
```

The comparison builds four decoders — this branch, the float library on the
adjacent worktree, and the two oracles — and runs all six captures through each.
