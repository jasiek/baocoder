# baocoder

A standalone, dependency-free C99 **encoder and decoder for AMBE+2 3600×2450**,
the half-rate vocoder DMR uses.

What makes it unusual: every quantiser table in it was read out of a Baofeng
DM-32UV firmware image rather than taken from another open implementation, and
the pitch quantiser is not a table at all — the radio computes it, so this does
too, with the radio's own constants. The framing and FEC are transcribed from
that firmware with the stock function and address cited at every block.

It grew out of `baofeng-dm32uv-reveng`, the Ghidra reverse-engineering project
for that radio, which holds the program database, the rename ledger and the
findings this codec rests on. mbelib and JMBE are used here only as oracles to
test against; no code or data from either ships in the library.

```
make          # libbaocoder.a + the ambe_decode / ambe_encode CLIs
make test     # 799 972 checks against known-good vectors, plus the integer-only check
make fixtures # regenerate tests/fixtures from upstream (needs network)
make tables   # re-extract the quantiser tables from the firmware image
```

```
./ambe_decode -k CDEFAB1234 tests/fixtures/dm32_arc4_1.frames out.wav
360 frames, 7.20 s: 111 silence, 1 erasure, 0.24 corrected bits/frame

./ambe_encode speech.wav frames.txt && ./ambe_decode frames.txt back.wav
```

## What is here

| Layer | File | Where it comes from |
|---|---|---|
| Golay(23,12) / (24,12) | `src/golay.c` | firmware `Golay23_Decode` `0x00015230`, `Vocoder_ComputeParityCode` `0x00022DF4` |
| 72-bit on-air frame ↔ 49-bit payload | `src/ambe_fec.c` | firmware `Vocoder_DescrambleVoiceFrame` `0x0001893C`, `Vocoder_DeinterleaveVoiceBits` `0x000230A4`, `Dsp_LcgSignScramble` `0x00021FE0` |
| 49 bits → model parameters | `src/ambe_params.c` | AMBE+2 model; firmware `Vocoder_DecodeFrameParameters` `0x0001994C` and the `Vocoder_*SpectralCodebook*` cluster as the behavioural reference |
| Parameters → 8 kHz PCM | `src/ambe_synth.c` | MBE synthesis; firmware `Vocoder_SynthesizeFrame` `0x00019DB8`, `Vocoder_SynthesizeVoiced/Unvoiced` `0x0001DE10` / `0x0001AFE0` |
| Fixed-point primitives (log2, pow2, sqrt, cos, divide) | `src/ambe_basop.c` | firmware `Math_Log2` `0x0001903C`, `Math_Pow2` `0x000191C0`, `Math_Sqrt` `0x00019364`, `Math_TableInterpLookup` `0x00019000`, `Math_SDiv` `0x00018D74` |
| Quantiser codebooks, voicing patterns, block lengths, and the primitives' coefficients | `src/ambe_tables_fw.c` | **extracted from the firmware image** by `tools/extract_tables.py` |
| Pitch and harmonic count | `ambe_pitch_from_b0` in `src/ambe_params.c` | **the firmware's own closed-form law**, constants read from the image |
| DMRA voice privacy (ARC4, AES-128/256) | `src/rc4.c`, `src/aes.c` | needed only to decode the encrypted test captures |
| Quantisation (params -> 49 bits) | `src/ambe_encode_params.c` | exact inverse of the decoder, searching the firmware's codebooks |
| Analysis (PCM -> params) | `src/ambe_analysis.c` | **ours**, not the firmware's — see below |
| Precision comparison against the float version | `tools/make_oracle.py`, `tools/compare_precision.py` | see `docs/fixed-point.md` |

The public API is `include/ambe.h`. The library has no dependencies at all — it
is integer-only and does not link `libm`, which `make check-fixedpoint`
asserts; `tools/mbe_ref.c` links mbelib but is only used to regenerate fixtures
and is not part of the library build.

## The quantiser tables come out of the radio

The vocoder's constant pool is not addressed in flash. It lives in the
`SRAM_SAHB` window at `0x18000000`, which the image populates at startup — so a
search of the image *at those addresses* finds nothing, which is what misled the
first version of this work. The mapping is

```
SRAM 0x18000000  <->  file 0x000636C0   (VA 0x0306F5C0)
```

fixed by two independent anchors that agree exactly: the 512-entry cosine table
the harmonic synthesiser indexes (`g_awSineTable512`, `0x18001630`) sits at file
`0x064CF0`, and `Vocoder_CodeSpectralCoefficients` `0x000220D4` loads its
codebook pointers from the literal pool at `0x00022480`, whose first entry
`0x18002090` is the codebook at file `0x065750`. Every pointer in that pool then
lands exactly on a table boundary:

| Table | SRAM | file | shape |
|---|---|---|---|
| block lengths | `0x18002030` | `0x0656F0` | 48 × uint16, nibble packed |
| PRBA24 | `0x18002090` | `0x065750` | 512 × 3, Q11 |
| PRBA58 | `0x18002C90` | `0x066350` | 128 × 4, Q11 |
| HOC b5 | `0x18003090` | `0x066750` | 32 × 4, Q11 |
| HOC b6 | `0x18003190` | `0x066850` | 16 × 4, Q11 |
| HOC b7 | `0x18003210` | `0x0668D0` | 16 × 4, Q11 |
| HOC b8 | `0x18003290` | `0x066950` | 8 × 4, Q11 |
| gain | `0x180038B4` | `0x066F74` | 32, Q11 |
| voicing patterns | `0x18003628` | `0x066CE8` | 128 × uint32, 2-bit crumbs |

The six codebooks are perfectly contiguous — `0x065750`–`0x066990`, 4 672 bytes.
What confirms the identification is not the numerical match but the bit widths
`Vocoder_CodeSpectralCoefficients` passes for them: **9, 7, 5, 4, 4, 3** —
exactly the widths of `b3`…`b8`.

The value tables are **signed 16-bit Q11**. Two others are packed, which is why
no byte-level search could ever have matched them:

* **Block lengths** — one uint16 per `L`, four 4-bit fields, least significant
  first, each biased `+2`: `Ji[i] = ((word >> 4*i) & 0xF) + 2`. Unpacked it
  reproduces mbelib's `AmbeLmprbl` for all 48 rows, each summing to `L`.
* **Voicing patterns** — 128 × uint32, two bits per band, most significant
  crumb first, and the *low bit of each crumb* is the band's voiced flag.
  `Vocoder_DecodeSpectralCodebookEntry` `0x00022DB4` indexes it with the `b1`
  field left-justified to seven bits, so for the 2450 mode's 5-bit `b1` the
  index is `b1 << 2`; `Vocoder_ExpandSpectralCodebookEntry` `0x0002AFAC` does
  the crumb expansion. All 32 rows reproduce mbelib's `AmbeVuv` exactly.

### What that says about mbelib

Comparing DVSI's shipping values against mbelib's independent reconstruction is
a two-way check, and it is not uniform:

* **PRBA24 and PRBA58 — 2 048 of the 2 368 values — agree to exactly half a Q11
  step.** mbelib's reconstruction of the gain codebooks is bit-for-bit what the
  radio ships, once rounded to the radio's precision.
* **The four HOC tables are up to 1.5 steps out**, and it is not a scale factor
  (fitting one by least squares still leaves 1.4 steps). mbelib's higher-order
  coefficients are very slightly wrong.
* The gain quantiser is within 1 step.
* mbelib's header carries **two** variants of `AmbeVuv`, one commented out as
  "alternate version". The image reproduces the active one for all 32 rows and
  the alternate for only 29 — so the active choice is right. That ambiguity was
  open in mbelib; the radio settles it.

## The synthesis window is not a firmware object

`Vocoder_ApplySynthesisWindow` `0x00029D1C` is a misnomer. It applies a ten-tap
antisymmetric FIR from SRAM `0x18003958` (file `0x067018`) whose taps are
`18411/(2k+1)` for `k = 0..9` — 1/1, 1/3, 1/5 … 1/19, the odd-harmonic series.
That is a Hilbert transformer, not a window. The stock synthesiser runs through
an inverse FFT (`Dsp_FftInverse` `0x00025704`) and has no overlap-add window
array at all.

mbelib's `Ws[321]` belongs to *its* time-domain sinusoid synthesis, which this
decoder also uses. It is a plain trapezoid — 56 zeros, a linear ramp in steps of
exactly 0.02, a flat top, the mirror image — with only three distinct slopes, so
`ambe_synth.c` generates it from that definition instead of carrying 321 copied
constants. The generated window agrees with mbelib's at all 321 points.

A structural search backs this up: scanning the whole image as int16, int32 and
float32 for a run of zeros followed by a smooth monotone ramp — the shape a
stored trapezoid would have — finds no such structure anywhere, in any width.

## Nothing is left unresolved

An earlier version of this project carried one gap: the 120-entry pitch
quantiser, which no search of the image could find. It was never a table. The
firmware evaluates the quantiser — `Vocoder_DecodePitchIndex` `0x00022B78` turns
b0 into log2 of the fundamental in Q12, `Vocoder_PitchFromLog2` `0x0002AD6C`
raises 2 to it, and `Vocoder_HarmonicCountFromPitch` `0x0002AD18` divides
`0x3B39C` by the result — so a search for stored values was looking for
something that does not exist. All three are transcribed in `src/ambe_params.c`.

The harmonic-count table was never a second gap either: `floor(0.4627 / f0)`
reproduces all 120 entries exactly, which `test_tables` asserts, and `0x3B39C /
2^19 = 0.462685` is that constant sitting in the firmware as an integer.

The `f0` law is now evaluated with the firmware's own four-term Taylor
polynomial for 2^x (`Vocoder_PitchFromLog2` `0x0002AD6C`) rather than with
`pow()`, and that closes the gap to mbelib rather than widening it: **4.8e-5**
relative against mbelib's table, where an exact `pow()` left 2.4e-3, and all 120
harmonic counts match. An earlier version of this README recorded a
one-index divergence at b0 = 17 and attributed it to the firmware; the
divergence was an artefact of evaluating the firmware's law with an exact
`pow()`, which the radio does not have. See `docs/fixed-point.md`.

The lead is the frame decoder at `0x0002033C`, which reads the `b0` field and
takes a per-mode descriptor carrying the field widths; that descriptor is the
thing to chase.

## Two things this settled about the firmware

Both were established here, not assumed, and are the reason the framing layer
can be called firmware-derived rather than firmware-inspired.

**1. The stock interleaver is bit-for-bit the schedule DSD and mbelib use.**
`Vocoder_DeinterleaveVoiceBits` `0x000230A4` is a plain 18×4 transpose,
`out[4k+j] = in[k+18j]`. Laid over the flat MSB-first concatenation
`c0(24) | c1(23) | c2(11) | c3(14)`, all 72 positions agree with the
`rW`/`rX`/`rY`/`rZ` tables the open-source DMR decoders use. A stripped vendor
firmware independently confirms the open reverse-engineering.

**2. `Dsp_LcgSignScramble` `0x00021FE0` is mbelib's AMBE PRNG exactly.**
The firmware computes `x = seed << 4`, then `x = x * 0xAD + 0x3619` in 16 bits
and flips the current bit while `x` is negative. mbelib's
`mbe_demodulateAmbe3600x2450Data` computes `pr[0] = 16 * foo`,
`pr[i] = (173 * pr[i-1] + 13849) mod 65536`, and takes bit 15. `0xAD` = 173 and
`0x3619` = 13849. The seed is the 12 data bits of codeword 0 in both.

The frame geometry falls straight out of `Vocoder_DescrambleVoiceFrame`
`0x0001893C`, which calls `Vocoder_ComputeParityCodeWord` with 12 parity bits
for the first 12-bit field and 11 for the second: 12 + 12 + 25 payload bits
become 24 + 23 + 25 = 72 on-air bits.

## The arithmetic is fixed point, like the radio's

The DM-32UV has no FPU. Its toolchain was configured soft-float and the vocoder
does not even call that: the DSP path runs ITU-T G.191 style basic operators
over a hand-rolled block-floating-point package in IRAM whose fifty callers are
the vocoder. So this library is integer-only too, and `make check-fixedpoint`
asserts it — `libbaocoder.a` references no libm symbol, links with no `-lm`, and
contains zero floating-point instructions.

The transcendentals are the radio's own, with coefficients read out of the image
by `tools/extract_tables.py` from a contiguous block at SRAM
`0x18001600`–`0x1800182F`:

| Primitive | SRAM | file | measured accuracy |
|---|---|---|---|
| `Math_Log2` `0x0001903C` | `0x18001600` | `0x64CC0` | 2.6e-03 abs — a four-term Taylor about 1/√2, not a minimax |
| `Math_Pow2` `0x000191C0` | `0x1800160C` | `0x64CCC` | 1.2e-04 rel |
| `Math_Sqrt` `0x00019364` | `0x18001618` | `0x64CD8` | 1.3e-03 rel |
| `Math_TableInterpLookup` `0x00019000` | `0x18001630` | `0x64CF0` | 6.1e-05 abs — 512-entry **cosine** table, linear interpolation |

`tests/test_basop.c` sweeps each across its whole input domain against libm;
those are the radio's accuracies, not this implementation's.

What that cost, over 2052 real frames against a double-precision reference: the
spectral envelope is a factor of thirty coarser (3.7e-04 against float's
1.3e-05, still inside the Q11 quantiser step of 4.9e-04 that the codec applies
to it anyway), the harmonic count and every voicing decision stay exact, and the
synthesised audio is **five decibels better** — 59.5 dB against float's 54.1 —
because the phase accumulator wraps exactly where a float loses mantissa bits,
and the overlap-add window's k/50 taps factor out instead of being rounded into
Q15. `docs/fixed-point.md` has the full comparison and the method.

## Testing: what "known-good pair" means here

The test corpus is six real Baofeng DM-32 transmissions — **2 052** on-air AMBE
frames under DMRA voice privacy (ARC4, AES-128 and AES-256), from
[`known-key-mbe-samples`](https://github.com/tylerwatt12/known-key-mbe-samples),
whose README records the DMR captures as coming from a Baofeng DM-32, the same
radio whose firmware supplied the tables. The reference side comes from two
independent implementations:

* **mbelib** — the reference decoder used by DSD, OP25 and SDRangel. Fixtures
  are generated by `tools/mbe_ref.c`, which links it.
* **JMBE / SDRTrunk** — a separate implementation again, via the capture's
  `expected.wav`.

| Test | What is compared | Result |
|---|---|---|
| `test_aes` | AES against the FIPS-197 Appendix B and C vectors, all three key lengths, plus the DMRA IV expansion | 92 checks |
| `test_golay` | the Golay code's defining property, exhaustively: 2048 syndromes ↔ 2048 error patterns of weight ≤ 3, all corrected; minimum distance 7; systematic encoding | 30 720 checks |
| `test_basop` | the firmware's log2, pow2, sqrt and cos swept across their **whole** input domains against libm, plus log2's coefficients against the Taylor series they turn out to be | 10 621 checks; cos 6.1e-5, log2 2.6e-3, pow2 1.2e-4, sqrt 1.3e-3 — the radio's accuracies, not this code's |
| `test_tables` | every quantiser value extracted from the image against mbelib's reconstruction, in Q11 steps; block lengths and all 32 voicing rows exactly; each block row summing to `L`; the firmware's pitch law over all 120 indices | 3 123 checks, PRBA worst **0.50 LSB**, voicing **32/32**, pitch `f0` **4.8e-5** / `L` **120/120** |
| `test_fec` | all 49 payload bits and both Golay error counts, per frame, against mbelib's FEC; the library's deinterleave against the firmware's formula written out again; encoder is the exact inverse of the decoder | 45 723 checks, 0.24 corrected bits/frame |
| `test_params` | per frame against mbelib: `L`, every voicing decision and the classification **exactly**; `w0`, gain and amplitudes bounded, since the decoder now uses the radio's law and Q11 tables where mbelib uses its float reconstruction | 12 450 checks; worst dev w0 **3.1e-6**, γ 2.7e-4, Ml 5.1e-3; **0** L divergences |
| `test_synth` | 16-band log-energy spectrum and level, per frame, against mbelib's PCM | mean band correlation **0.989**, level ratio **0.999** |
| `test_e2e` | on-air bytes → FEC → decrypt → audio for all six captures, against JMBE's `expected.wav` | mean band correlation **0.966** across six captures |
| `test_encode` | decode 2 052 real frames from six captures to parameters, re-quantise, demand the radio's own bits back | **1 354/1 664** voice frames bit-identical; the rest differ only where the index is unrecoverable, and all re-decode to identical parameters |
| `test_encode_sweep` | synthesised frames stepping every codebook by coprime strides | **every entry of all nine codebooks**, 48/48 harmonic counts, 4 096 frames |
| `test_encode_pcm` | analyse audio whose true pitch is known because it came from the radio's bitstream | pitch within 4 quantiser steps on **173/191**, **1** octave error, level ×0.93 |

`test_params` is the load-bearing pipeline test. The AMBE spectral envelope is
coded differentially against the previous frame, so one wrong bit position or
blend coefficient diverges within a few frames and never recovers. Running 360
consecutive frames of real speech through it and holding `w0`, `L` and every
voicing decision exact is a far stronger statement than any single-frame vector.

Since the decoder now uses the radio's Q11 tables and mbelib uses its own float
reconstruction, the gain and amplitude chain *cannot* agree exactly. Those are
bounded at the measured worst case instead — γ within one quantiser step
(2.7e-4), spectral amplitudes within 0.7% — and `test_tables` is where the
tables themselves are checked value by value.

### Why the audio is not compared sample by sample

MBE synthesis is deliberately stochastic: every harmonic above `L/4` gets a
random phase offset and unvoiced bands are filled with randomly phased
multi-sine noise. mbelib calls `rand()`; this decoder uses a seeded xorshift so
its output is reproducible run to run. Two correct implementations therefore
never produce identical waveforms, and the tests compare spectra and levels.
`test_synth` also hashes this decoder's own output to catch accidental
non-determinism.

### The decryption is itself evidence

Three DMRA privacy modes are reconstructed, in `src/rc4.c` and `src/aes.c`:

* **ARC4** (ALGID 0x21) — RC4 over `key ‖ MI`, 256 keystream bytes discarded.
* **AES-128/256** (0x24 / 0x25) — OFB from a 128-bit IV that the 32-bit message
  indicator is expanded into by an LFSR with taps {32, 22, 2, 1}, with the first
  cipher block discarded.

All three consume 49 bits and skip 7 per frame, re-keyed every 18-frame
superframe. The cipher itself is pinned by the FIPS-197 vectors in `test_aes`;
the keying above it is validated by evidence that owes nothing to this project —
**speech has a continuous pitch track**. The mean step between consecutive
frames' `b0` runs 3.9 to 16.4 across the six captures, where randomly keyed bits
give 39.4, the expected mean absolute difference of two uniform draws over the
120 voice indices.

The silence-descriptor rate was tried first as the discriminator and rejected:
it ranges from 4.8% to 30.8% here, because some of these captures are continuous
speech, so it does not separate cleanly from chance. Recording the rejected
statistic matters as much as the accepted one.

## Fixtures

`tests/fixtures/` holds only derived vectors (frames, payload bits, reference
parameters and reference PCM, ~470 KB). The upstream sample repository carries
no licence, so it is fetched into `third_party/` by `make fixtures` rather than
vendored; `test_e2e` skips its audio comparison when it is absent and still
runs everything else.

## The encoder

Both directions work. The channel-coding half was already there —
`ambe_fec_encode` is tested as the exact inverse of the decoder — and the two
new pieces are quantisation and analysis, which sit at very different confidence
levels.

**Quantisation (`ambe_encode_params.c`) is the firmware's, and is verified
bit-exactly.** It inverts the decoder exactly and searches the extracted
codebooks for the nearest entry, which is what the stock code does too:
`Vocoder_CodeSpectralCoefficients` `0x000220D4` and
`Vocoder_CodebookVectorLookup` `0x0002369C` take a direction flag, and on the
encode side the lookup searches and emits an index with `Dsp_UnpackBitsAdvance`
instead of reading one with `Dsp_PackBitsAdvance`.

Inverting the envelope stage is the only subtle part. With `P[l]` the weighted
resampled previous frame and `D[l] = log2Ml[l] - P[l] + mean(P)`:

```
D[l]        = Tl[l] - mean(Tl) + gamma - 0.5*log2(L)
mean(D)     = gamma - 0.5*log2(L)        -> gamma recovered
D - mean(D) = Tl - mean(Tl)              -> Tl up to its mean
```

`mean(Tl)` is genuinely not transmitted — adding a constant to every `Tl`
cancels in the decoder. That costs nothing, because the constant only moves
`Gm[1]`, which the codec pins to zero: adding `c` to each block's DC adds `c` to
every `Ri`, and `sum_i cos(pi*(m-1)*(i-0.5)/8) = 0` for every `m > 1`. So
`Gm[2..8]` — all that b3 and b4 carry — is exactly recoverable.

**Analysis (`ambe_analysis.c`) is ours, not the firmware's.** The stock analyser
is a dense fixed-point multi-candidate search spread across
`Vocoder_SelectVoicingCandidate` `0x000177F0`, `Vocoder_RefinePitchEstimate`
`0x00025A60` and `Dsp_WindowAndComputeFft` `0x00019B6C`; what is here is a
conventional MBE estimator producing the same parameter set — normalised
cross-correlation pitch over lags 20..123 (exactly the span the pitch quantiser
covers), per-band voicing from harmonic energy concentration, and RMS spectral
amplitudes from a Hamming-windowed 256-point DFT. It is honest work, not a
transcription, and the tests are scaled to that.

## How much of the codec the tests actually reach

Worth stating plainly, because "360 real frames" sounds like more coverage than
it is. Seven seconds of one person talking visits only part of the quantiser
space:

| field | codebook | entries reached by the real capture |
|---|---|---|
| b3 | PRBA24, 512 | 181 — **35%** |
| b4 | PRBA58, 128 | 112 — 88% |
| b2 | gain, 32 | 16 — **50%** |
| b0 | pitch, 128 | 52 — 41% |
| L | 48 rows | 32 — **67%** |

More speech does not fix this: real audio concentrates on common entries and
never reaches the tail. So `test_encode_sweep` synthesises frames that step each
codebook by a coprime stride, covering **every entry of all nine codebooks and
all 48 harmonic counts** — coverage no recording can provide. The real captures
remain the more meaningful test, because they are bits a radio actually
transmitted; the sweep is what makes the coverage complete.

Adding the second capture was not just more of the same: it contained frames
with small enough `L` that some prediction blocks carry no higher-order
coefficients at all, a path the first capture never exercised.

Still thin, and honestly so: **2 tone frames and 3 erasure frames** in 2 052.
Those paths are structurally simple — classify and mute — but they are barely
exercised by real data.

## Limitations

* The analyser is a conventional MBE estimator, not the firmware's search, so
  encoded audio is not bit-identical to what the radio would produce from the
  same input — only the quantiser below it is exact. It is fixed point like the
  rest of the library, but converted rather than transcribed;
  `docs/fixed-point.md` lists the stock functions a transcription would need.
* b1 is not uniquely recoverable when a frame's voicing is degenerate: the
  32-entry table has only 13 distinct 8-band patterns, and bands no harmonic
  maps to are never read.
* b5..b8 are not recoverable for a prediction block with no higher-order
  coefficients (`Ji <= 2`); the encoder emits 0, which is what the decoder
  ignores.
* Tone and erasure frames are barely represented in the corpus.
* 3600×2450 (DMR) only. The 3600×2400 D-STAR variant and the IMBE 7200×4400
  used by P25 Phase 1 are not implemented.
* Tone frames (`b0` = 126/127) are classified and muted, not synthesised.
* No soft-decision FEC.
* The synthesis window is this decoder's own (a trapezoid, generated); the
  firmware synthesises through an inverse FFT and has no equivalent — see above.

## Licence and provenance

The code here is ISC. It relies on prior open reverse-engineering, and says so:

* **mbelib** — Copyright (C) 2010 mbelib Author, ISC. The correctness oracle
  throughout. No mbelib data ships in this decoder.
* **known-key-mbe-samples** — the DM-32 capture used as the test corpus.
* **DSD / dsd-fme** — the DMRA Enhanced Privacy keystream construction was
  cross-checked against `dsd-fme`'s implementation.

AMBE and AMBE+2 are DVSI trademarks and the algorithms are patented in some
jurisdictions. This is interoperability research on a radio the author owns.
