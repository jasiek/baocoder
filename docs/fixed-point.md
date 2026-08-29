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
signed 32×32 form nor `mulsh`'s accumulating variants:

```
i32_r_sop = 0b100011  pcode 0b00001  ->  muls     42 sites
i32_r_sop = 0b100100  pcode 0b00010  ->  mulsha   42 sites
i32_r_sop = 0b100100  pcode 0b00100  ->  mulshs   12 sites
```

Ghidra's flow analysis stops dead at an instruction it cannot decode, so every
function containing one is silently truncated. Structure suggested the
encodings — each has the shape of the family one sop lower, and the C-SKY V2
multiply group pairs unsigned and signed families adjacently — and a table
learned from 884 register-ALU instructions that Ghidra *does* decode confirmed
none of the three appeared in the implemented set.

But structure is not evidence about *semantics*, and the reverse-engineering
project's patches README says so in as many words: it had recorded this whole
family as deliberately unpatched, because inventing p-code for a MAC
instruction produces confidently wrong decompilation. What made these three
different is that they could be checked. On bytes Ghidra had never reached the
first decodes as `muls r0,r2 / mfhi r12 / mflo r0` — the accumulator read that
has to follow a multiply into `hi:lo` — and with all three, the FFT below
reproduces a reference DFT. Wrong semantics do not produce a working FFT.

The signed family's *accumulating* forms follow just as obviously by symmetry
and are deliberately left out. Nothing validated exercises them, and the first
has 56 sites — more than any encoding in the patch. `docs/patches/csky-muls.patch`
carries the fix and says which forms are evidenced; it lives alongside the
reverse-engineering project's `csky-movih-truncation.patch`, which is where the
canonical copy is.

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

### The transform, transcribed

`src/ambe_fft.c` is that chain, and the analyser now uses it in place of the
O(N²) DFT it had. Its whole spectral front end is the radio's: the radio's
window, the radio's geometry, the radio's transform.

`tests/test_fft.c` holds it against a reference DFT applying the same window.
The measure is a **ratio, not a correlation**, and the choice carries the
argument: a block-floating-point transform has an arbitrary common scale, so
if it is right then the difference between its dB spectrum and the reference's
is a *constant*. It is — constant to **0.56 dB** across the bins within 40 dB
of the peak, with all ten peak bins exactly right.

Three transcription errors were caught by that test and none was visible by
reading:

* Ghidra renders `mulsh`'s sign extension as `& 0xffff` on the operands, so the
  squared magnitudes became unsigned products and overflowed to negative.
* The stock buffer pointer is `ushort *`, so `+ (1 << size_bits)` advances half
  as far as the same expression on the int32 complex words. Transcribed
  straight it overran by 2× and corrupted everything downstream.
* In the real-spectrum unpacking the second output's halves are crossed — the
  real combination is written to the imaginary slot and vice versa. Missing
  that mirrored the spectrum about N/2, which looks entirely plausible until
  you check which bin the peak is in.

That last one is the argument for testing transcriptions against what they
should compute rather than reviewing them. A mirrored spectrum has the right
shape, the right dynamic range and the right noise floor.

One bin is derived rather than transcribed, and is marked so in the source. Bin
N/4 is the packed real FFT's self-conjugate point, where the general
combination collapses to `X[N/4] = conj(Z[N/4])`. The stock code reaches it
through a degenerate pass of the unpacking loops whose iteration count Ghidra
does not recover — the argument is left in a register — and both candidate
counts leave that one bin several dB wrong against the reference. It is written
from the identity instead.

### The pitch decision that turned out not to be one

`0x000177F0` had been renamed `Vocoder_SelectPitchCandidate`, correcting a
still earlier `Vocoder_SelectVoicingCandidate`. Both names are wrong, and so
was the plan to transcribe it. **It is a DTMF decoder.**

Reading its tables settled the arithmetic first. Four pointer pairs in its
literal pool land in one contiguous SRAM block, and a row's weights are the
four shorts immediately *before* its offsets, not after. An offset is the base
of a five-tap window — `Dsp_FindMaxEnergyBlockIndex` `0x000177B0` sums five
consecutive int32 at each of a row's four offsets and keeps the largest — and
`Vocoder_CandidateEnergyPassesThreshold` `0x00017670` builds the first moment
`M = 2·Σ k·block[k]` over that window and tests

```
| 2·E·off + M − T |  <  (thresh · T) >> 20,    T = (2·W·E) >> 9
```

which divided by `2E` is `| off + centroid − W/512 | < (thresh/2^20)·(W/512)`.
So `W` is a nominal position in Q9 and the tolerance is relative.

What the positions *are* comes from this project's own FFT transcription.
`Vocoder_ProcessFrame` `0x00016E04` passes `pCtx + 0x18` as the first argument
of `Dsp_WindowAndComputeFft` — which `ambe_fft.c` has as `magsq_out`, writing
`shift + (fft_size >> 1)` = 129 int32, exactly the `0x204` bytes to the next
history slot. The array is the **|X|² half-spectrum of a 256-point FFT at
8 kHz**, so an index is a bin at 31.25 Hz. On that basis:

| row | `W/512` as a bin | frequency |
|---|---|---|
| `0x1800140C` | 22.305, 24.641, 27.264, 30.111 | **697, 770, 852, 941 Hz** |
| `0x1800141C` | 38.688, 42.752, 47.264, 52.256 | **1209, 1336, 1477, 1633 Hz** |

The DTMF row and column groups. Solving each of the eight for the sample rate
that would make it exact gives 7999.7–8000.2 Hz. The 16-entry table at
`0x180013E4`, indexed `[(i + j*4) & 15]`, then reproduces the DTMF keypad
exactly — sixteen of sixteen, `A`–`D` = 10–13, `*` = 14, `#` = 15 — and the
thresholds are a frequency tolerance: ITU-T Q.24 accepts within ±1.5 % and
rejects beyond ±3.5 %, and these are 2.00005 % and 1.59998 %.

A second 4×4 grid runs through the same path tagged `+ 0x90`, at 606.0/671.7/
743.5/819.9 × 1051.9/1161.5/1296.5/1429.5 Hz. It is left unidentified. It has
DTMF's geometric shape at 0.870× its frequencies but is not DTMF at another
rate — forcing that fit spreads the sample rate over 9114–9202 Hz where the
real grid fits to 0.005 %.

The useful part for this branch is negative: **`0x000177F0` is not part of the
speech analyser and this library does not need it.** It runs on the analyser's
spectrum only because that spectrum is already in the buffer.

### Two more instructions, and where the analyser actually lives

`Vocoder_AnalyzeSpectrum` `0x000205B8` is the speech analyser, and its callee
list says so: `Vocoder_RefinePitchEstimate` `0x00025A60`,
`Vocoder_AnalyzeSubbandSpectrum` `0x00023AA8`,
`Vocoder_SelectSpectralSubbands` `0x0002AA20` and `Math_Log2` — pitch,
subbands, voicing, amplitudes. `Vocoder_UpdateEnergyAndHistory` `0x000217CC`
calls four things, runs *after* the analysis, and is envelope and history
state maintenance. It is not the way in.

Reading those needed two more C-SKY instructions, both checked by result and
both in the reverse-engineering project's `docs/patches/`:

* **`mthi`/`mtlo`** — the write side of the HI/LO accumulator group, 44 sites.
  Confirmed by `FUN_0001ACCC` disassembling to
  `movi r5,0 / mthi r5 / mtlo r5 / <loop> / mfhi / mflo`: an accumulator clear
  before a MAC loop, the mirror of the read that confirmed `muls`.
* **`mulsa`** — the accumulating signed multiply, 56 sites, which that project
  had deliberately refused for want of a checkable result. One exists:
  `FUN_0001ACCC` uses it with a multiplier of 1, i.e. as a widening 64-bit add,
  and the image already contained two MAC-free implementations of that same
  sum — `Dsp_SumInt32Array` `0x00017634` and the inlined loop in
  `Vocoder_UpdateEnergyAndHistory`. With `mulsa` as accumulate, `FUN_0001ACCC`
  decompiles to exactly those, with zero warnings.

`Vocoder_NormalizeSpectralArrays` `0x0002A70C`, recorded above as "a different
blocker from the one that has been fixed", went 28 → 168 lines. The blocker was
`mthi`/`mtlo`.

### The voicing rule, read

`Vocoder_SelectSpectralSubbands` `0x0002AA20` is the voicing mask, and it now
reads cleanly. Eight bands, the spectrum advancing 32 int16 per band, with
`pitch` from `Vocoder_RefinePitchEstimate`:

```
E_total = Math_ArraySum(spectrum + sVar7*2, 0x20 - sVar7),  sVar7 = pitch/1024 + 1
E_harm  = Dsp_SumSpectralBand(spectrum, pitch, 6, 0x40)
v       = Math_OneMinusRatioQ15(E_harm, E_total)
voiced when v < 0x199A
```

`Dsp_SumSpectralBand` `0x000263D0` walks the band **on the harmonic grid** —
step `pitch*128` in 16.16, starting at `1.0 + 0.75*step` — summing a window at
each harmonic. So the measure is the one this file already described from
first principles: how much of a band's energy sits at the harmonic peaks.

**The threshold is not what it looks like.** `0x199A/0x8000 = 0.2000` exactly,
which invites "voiced below 0.2". `Math_OneMinusRatioQ15` `0x0002A6AC` returns
`0x7FFF − num/den` — the *complement* — so the rule is

> voiced when **E_harm / E_total > 0.80**.

This library uses 0.60 for the same decision. The two are not directly
swappable — the firmware's denominator starts at a pitch-dependent bin and its
numerator uses a specific harmonic window — but 0.60 was a guess and 0.80 is
the radio's.

**The octave guard is per band and energy-based.** When `pitch > 0xCCD` each
band also evaluates one octave down (`nBand = 5`, `sVar7 = pitch/2048 + 1`) and
`Math_FloatGreater` keeps whichever hypothesis carries more energy. This
library instead applies one global rule — prefer the shortest lag within a few
percent of the best — which is a different algorithm, not a tuning difference.

The eight bits pack **MSB first**.

### What that rule is worth here, measured

The firmware's constant is 0.80 and this library's is 0.60, on what looked like
the same measure. Swapping it would have been the obvious move. It is wrong,
and finding out why turned up something more useful.

`tests/test_encode_voicing.c` measures the decision the same way
`test_encode_pcm` measures pitch — decode a real frame, re-analyse its audio,
compare the recovered `b1` with the transmitted one. **67% of the corpus's
bands are voiced**, so the number to beat is not 50% but the trivial answer
"voiced every time". Sweeping the threshold over 1117 frames:

| threshold | b1 exact | bands correct | always-voiced |
|---|---|---|---|
| 0.01 | 22.1 % | 68.73 % | 67.26 % |
| 0.20 | 20.0 % | 68.24 % | 67.26 % |
| 0.40 | 15.6 % | 67.98 % | 67.26 % |
| **0.60 (shipped)** | **5.5 %** | **48.04 %** | 67.26 % |
| 0.80 (the firmware's) | 0.5 % | 34.95 % | 67.26 % |

Three things fall out, and only the first is about the constant.

**The firmware's 0.80 does not transfer**, and badly — 35%. So the two measures
are not the same quantity despite the same shape: the radio's denominator
starts at a pitch-dependent bin and its numerator sums a window on the harmonic
grid, where this library sums peak bins over a fixed band. A different measure,
not a different constant.

**The shipped 0.60 is worse than answering "voiced" every time** — 48% against
67%. That is not a tuning preference; the decision is actively costing
accuracy.

**And retuning does not rescue it.** The plateau below 0.40 is flat — 68.0% to
68.7% all the way down to 0.01, where the rule degenerates into never calling a
band unvoiced — so the entire discriminative power of this measure is about
1.5 points, a kappa near 0.04. It carries essentially no information.

The constant is therefore **unchanged**, and the reason is a coupling worth
recording: voicing and amplitude are entangled here, because an unvoiced
harmonic is scaled by 1/0.2046 at the amplitude step. At threshold 0.20 the
round-trip level falls from ×0.99 to **×0.887**, and no value of
`AMBE_ML_SCALE_Q16` restores it — sweeping that constant from 1000 to 65000
saturates at 0.887. A decibel of level is a real cost; one point on a decision
with no information is not a real gain.

`test_encode_voicing.c` is kept as a **characterisation** test: it pins the
current behaviour and prints the baseline beside it every run, so the gap is
visible rather than buried. The fix is to transcribe
`Vocoder_SelectSpectralSubbands`, not to move a number.

### Why it does not port, and what porting it would take

The obvious next step was to transcribe the firmware's rule. Tracing its inputs
first turned up the reason the constant failed, and it is structural rather
than numerical.

**The array the rule reads is not the main transform.**
`Vocoder_AnalyzeSpectrum`'s `local_6e4` is `uint[128]` — 512 bytes, read by the
voicing code as **256 int16 in 8 bands of 32** (`pSpectrum += 0x40` per band,
and `Dsp_SumSpectralBand`'s `nWidth = 0x40` yields 32 as its loop bound). It is
filled by **eight 64-point sub-band FFTs** (`Dsp_FftForward(…, 6, 0)`) inside a
nested loop that shifts per-band exponents from context state. The radio
decides voicing on a sub-band filterbank; this library uses peak bins of one
256-point spectrum. That is why 0.80 scored 35% here — a different spectral
representation, not a different normalisation.

**The pitch argument's format is pinned, and without needing
`Vocoder_RefinePitchEstimate`** — the one function still truncated.
`Dsp_SumSpectralBand` computes `iVar2 = (pitch << 16) >> (15 − nBand)`, so at
`nBand = 6` the harmonic step is `pitch/512` in 16.16 bins. A harmonic step for
a 256-point transform at 8 kHz is `f0_turns × 256`, so

> `pitch = f0_turns × 2^17` — f0 in **Q17 turns/sample**, which is this
> library's `f0` (Q19) shifted right by two.

Three independent checks agree:

* `nBand = 5` gives `pitch/1024`, exactly half the step — the octave
  hypothesis, confirmed by arithmetic rather than by shape.
* `sVar7 = pitch/1024 + 1` lands in **2..7** across the codec's whole f0 range
  (0.0081–0.05 turns/sample), inside the 32-bin band with `0x20 − sVar7` always
  positive.
* The octave gate `0xCCD` becomes `3277/2^17 = 0.025000` turns/sample =
  **exactly 200.0 Hz**. A round threshold in Hz, which no other Q format
  produces.

**So the job is bigger than the rule.** The rule itself is fully read and is
the small part. What it needs underneath is the sub-band filterbank that fills
`local_6e4`: `Vocoder_AnalyzeSpectrum` `0x000205B8`'s band-construction loop
(747 lines, 0 warnings) and `Vocoder_AnalyzeSubbandSpectrum` `0x00023AA8` (764
lines, no `halt_baddata`). Both are readable now; neither is small. Until that
lands, the voicing decision here stays the guessed one, and
`tests/test_encode_voicing.c` keeps printing what it is worth.

### The filterbank, mapped

The band-construction loop in `Vocoder_AnalyzeSpectrum` is what stands between
this library and the firmware's voicing rule. It is now mapped, which is the
transcription spec even though the C is not written:

**Eight bands, 50 % overlap-add.** The outer loop runs 0..7 advancing the write
pointer by **16 int32** while each band *writes* **32 int32**, and the write is
an exponent-aligned add, not a store:

```
*(int *)(puVar14 + i) = (*(int *)(puVar20 + i) >> shift) + *(int *)(puVar13 + i)
```

`8 × 16 = 128` is exactly the `local_6e4[128]` the voicing rule reads; the
eighth band's tail runs into the adjacent scratch buffer, which the code then
uses deliberately.

**Two sub-frames per band**, combined the same way. Each is a **58-sample**
segment (`0x3A`), windowed by a folded 58-tap table, zero-padded into a
**64-point** transform (`Dsp_FftForward(…, 6, 0)`), then `|X|² × 2` over
**32 bins** with bins 0 and 1 zeroed. Per-band block-float exponents go to
`asStack_40[8]` — `Vocoder_SelectSpectralSubbands`'s fourth argument.

Input strides: per sub-frame 49 and 11 samples; per band 22 and 98.

**A third window table**, at SRAM `0x18001484..0x180014BC` (file `0x064B44`):
29 half-taps applied symmetrically, matching the 58-sample segment. Its
edge/peak ratio is 0.0807 against a Hamming's 0.08 — but it is **not** a
Hamming. The best raised-cosine fit is `M = 57`,
`w = 0.5457 − 0.4543·cos(2πn/57)`, worst residual **360 LSB** (1.1 %), against
the **1 LSB** the 199-point window matches its Hamming to.

So two of the analyser's three windows are non-analytic, and if they are
transcribed it should be as verbatim bytes — which is what this project does
with every table anyway, and why the Hamming identification was always a bonus
check rather than the mechanism.

**What is left to write** is the C for the loop above plus its exponent
bookkeeping, and `Vocoder_AnalyzeSubbandSpectrum` `0x00023AA8` (764 lines,
readable) which runs ahead of it. The voicing rule then drops on top and
`tests/test_encode_voicing.c` becomes a pass/fail check instead of a
characterisation.

### A second analysis window

`Vocoder_AnalyzeSpectrum` runs its own transform before pitch and voicing:
`Dsp_WindowAndComputeFft(…, PTR_DAT_00020E5C, -7, 0xFF, 0, …, 8, 0)` — **255**
samples, not the 199 this library uses, through a table at SRAM `0x18000FA8`
(file `0x064668`): the 128 shorts sitting immediately before the 199-point
Hamming, so the block is contiguous the way the others were. Peak 28193.

It is monotone edge-to-centre with a smooth bell-shaped first difference, but
it is **not** a cosine-family window — a 3-term least-squares fit leaves about
1000 LSB of residual against Hamming, Hann and Blackman forms — and there is an
unexplained rate break between entries 5 and 6. It is recorded as measured
rather than named; if it is transcribed it should be as verbatim bytes, which
is what this project does with every other table anyway.

### What remains

1. `Vocoder_AnalyzeSubbandSpectrum` `0x00023AA8`, 764 lines and clean.
2. `Vocoder_AnalyzeSpectrum` `0x000205B8` itself, 747 lines and clean — the
   band-energy loop at its tail (groups of 5 then 8 bins, scaled by `0x4800`)
   is the amplitude path.
3. `Vocoder_RefinePitchEstimate` `0x00025A60` still has one 164-byte gap, at a
   `0b100110`/`0b00100` encoding with the `mfhi`/`mflo` shape — probably
   `mflos`, a *saturating* accumulator read. Left alone on purpose: it means
   inventing a saturation rule, and unlike `mthi`/`mtlo` and `mulsa` there is
   nothing in the image to check the guess against.

So the voicing rule is now evidence rather than a guess, and the pitch
estimator is the one stage still partly closed.

So the pitch estimator is still partly closed, but the subband analyser is
open, and this library's own pitch search is the weakest thing in it
(173/191 within four quantiser steps, one octave error).

One trap, recorded because it cost the most time: these tables are nonsense
against `DM32UV_L01_048.bin`. The SRAM image that `BASE = 0x0636C0` maps is the
one in `DM32_L01_048_20250821.bin`, which is what `tools/extract_tables.py`
reads. Same mapping, different file.

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
