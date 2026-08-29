#!/usr/bin/env python3
"""Extract the AMBE+2 quantiser tables from the DM-32UV firmware image.

    tools/extract_tables.py /path/to/DM32_L01_048_20250821.bin > src/ambe_tables_fw.c

or, from the top of the tree:  make tables FIRMWARE=/path/to/image.bin

HOW THE TABLES WERE LOCATED
---------------------------
The vocoder's constant pool is not addressed in flash: it lives in the
SRAM_SAHB window at 0x18000000, which the image populates at startup.  The
mapping was fixed by two independent anchors that agree exactly:

  * the 512-entry cosine table the harmonic synthesiser indexes (g_awSineTable512,
    SRAM 0x18001630) is at file 0x064CF0, and
  * Vocoder_CodeSpectralCoefficients 0x000220D4 loads its codebook pointers from
    the literal pool at 0x00022480, which reads 0x18002090 for the first codebook;
    that codebook is at file 0x065750.

Both give   SRAM 0x18000000  <->  file 0x000636C0   (VA 0x0306F5C0).

Every pointer in that literal pool then lands exactly on a table boundary:

    0x18002030 -> 0x0656F0   block lengths, 48 x uint16, nibble packed
    0x18002090 -> 0x065750   PRBA24  512 x 3
    0x18002C90 -> 0x066350   PRBA58  128 x 4
    0x18003090 -> 0x066750   HOC b5   32 x 4
    0x18003190 -> 0x066850   HOC b6   16 x 4
    0x18003210 -> 0x0668D0   HOC b7   16 x 4
    0x18003290 -> 0x066950   HOC b8    8 x 4
    0x180038B4 -> 0x066F74   gain quantiser, 32 entries
                             (from Vocoder_LookupAndBlendGain 0x0002AE70's pool
                              at 0x0002AFA8)

The six codebooks are perfectly contiguous, 0x065750..0x066990, 4672 bytes.
The bit widths Vocoder_CodeSpectralCoefficients passes for them - 9, 7, 5, 4, 4,
3 - are exactly the widths of b3..b8, which is what confirms the identification
rather than the numerical match alone.

FORMAT
------
All value tables are signed 16-bit, Q11 (scale 2048).  Fitting each against
mbelib's floats by least squares recovers a scale of 2048.00 and a worst-case
residual of half an LSB, i.e. the firmware's values and mbelib's reconstruction
agree to the firmware's own precision.

THE MATH-PRIMITIVE CONSTANTS
---------------------------
The radio has no FPU and its vocoder never calls the soft-float library: the
DSP path is ITU-T G.191 basic operators over a block-floating-point package in
IRAM.  Those primitives' coefficients live in the same SRAM window as the
codebooks, and each one is reached through the function's literal pool:

    Math_Log2 0x0001903C  pool 0x000190BC -> 0x18001600 -> file 0x064CC0
    Math_Pow2 0x000191C0  pool 0x0001927C -> 0x1800160C -> file 0x064CCC
    Math_Sqrt 0x00019364  pool 0x000193DC -> 0x18001618 -> file 0x064CD8
    Math_TableInterpLookup 0x00019000
                          pool 0x00019038 -> 0x18001630 -> file 0x064CF0

The Scaled variants - Math_Log2Scaled 0x000190C0, Math_Pow2Scaled 0x00019280,
Math_SqrtScaled 0x000193E0 - read the *same* three coefficient blocks through
their own pools, so there is one polynomial each and two entry points.

0x18001600..0x1800182F is perfectly contiguous, which is the same structural
confirmation the codebook identification rested on, and the values identify
themselves: Log2's leading coefficient is 1/sqrt(2), Pow2's fifth is 0x58B9 =
ln2 in Q15, and Sqrt carries two six-coefficient sets selected by the parity of
the normalisation shift.  The 512-entry table is a *cosine* table despite the
g_awSineTable512 name it carries in the reverse-engineering ledger: it matches
32767*cos(2*pi*i/512) to within one LSB at every one of its 512 points.

THE FFT'S TABLES
----------------
Dsp_WindowAndComputeFft 0x00019B6C windows 199 samples into a 2^8 transform,
which Dsp_FftForward 0x000256D0 runs as a 128-point *complex* FFT over the 256
real samples packed as even/odd into re/im.  Its three tables:

    0x18001A30  file 0x0650F0  256 complex pairs, exp(-j*2*pi*k/512) in Q15,
                               reached from Dsp_FftStageButterfly's pool at
                               0x00025220.  Matches cos and -sin to 1 LSB.
    0x180014E0  file 0x064BA0  the N=32 bit-reversal permutation
    0x18001514  file 0x064BD4  the N=128 bit-reversal permutation

The two permutation tables are a count followed by delta-coded swap pairs: the
stock code walks two pointers, advancing each by the next delta (in halfwords)
and swapping the 32-bit words they land on.  Decoded that way they reproduce
the bit-reversal permutation exactly - 12 pairs for N=32, 56 for N=128 - which
is what identifies them.

Both butterfly kernels and the permutation are only readable at all because of
docs/patches/csky-muls-family.patch: the processor module was missing the two
multiply families they are built from, and Ghidra truncates a function at the
first instruction it cannot decode.

THE ANALYSIS WINDOW
-------------------
Vocoder_ProcessFrame 0x00016E04 - the speech analyser's per-frame entry point,
which takes PCM in - hands Dsp_WindowAndComputeFft 0x00019B6C a window table
from its literal pool at 0x00016F38, with nInLen = 199 and an FFT size of 2^8.
The pool reads 0x180010A8, file 0x064768.

The stock code stores only half the window and folds it symmetrically, so the
table is 100 entries for a 199-point window.  It is a Hamming window: scaled by
its own peak, 29883, it reproduces 0.54 - 0.46*cos(2*pi*i/198) at all 100
points to within a count.  tests/test_tables.c asserts that.

THE SUB-BAND FILTERBANK'S TABLES
--------------------------------
Vocoder_AnalyzeSpectrum 0x000205B8 runs two filterbanks, and both their tables
sit in the same contiguous SRAM block, 0x18001454..0x180014BD:

    0x18001454  file 0x064B14  16 x int16, half of a symmetric 32-tap window
    0x18001474  file 0x064B34   8 x int16, a 7-tap symmetric FIR plus a pad
    0x18001484  file 0x064B44  29 x int16, half of a 58-tap window

The first two belong to Vocoder_AnalyzeSubbandSpectrum 0x00023AA8, which is a
16-channel analysis filterbank.  It reads the 32-tap window through two
pointers into the same table (0x000240A8 and 0x00024920, both 0x18001454) and
applies it folded - sample j takes tap j and sample 31-j takes it again - then
hands the 32 windowed samples to Vocoder_SubbandSumDifference.  The window is
not analytic: the best raised-cosine fit leaves 1309 LSB against a peak of
30913.  What identifies it is its sum, 524290, which is 2**19 + 2 - a DC gain
of exactly 16 in Q15, one per output channel.

The 7-tap FIR is read at 0x00024EAA and applied with a stride of 16 shorts, so
it runs down one channel of the 16 interleaved ones and emits 11 new samples
per channel per frame.  Its taps are [-4456, 3034, 19608, 29164, 19608, 3034,
-4456] and they sum to 65536 exactly, which is unity for the >> 16 the code
applies - that is what identifies it, and the trailing zero is padding to a
word boundary.

The third belongs to the eight-band loop in Vocoder_AnalyzeSpectrum itself,
which reaches it through 0x00020B3C holding 0x180014BC and walks *downwards* to
0x18001482 (held at 0x00020B38), i.e. 29 entries applied symmetrically to a
58-sample sub-frame, stored edge-first.  Because nothing points at the table's
own start address, a byte search for it finds nothing.  It is not a Hamming
window despite an edge/peak ratio of 0.0807: the best raised-cosine fit is
M = 57, w = 0.5457 - 0.4543*cos(2*pi*n/57), and it leaves 360 LSB.

THE 32-POINT REAL DFT
---------------------
Vocoder_SubbandSumDifference is not an FFT.  It folds the 32 windowed samples
into 15 half-sums and 15 half-differences, then multiplies them by a stored
matrix at 0x180039EC (file 0x0670AC, 480 x int16, bounded above by 0x18003DAC
which the loop tests against): 15 rows of 16 cosines followed by 16 sines.  Bin
0 is the plain sum, written before the loop with a zero imaginary part.

The matrix is round(32768*cos(2*pi*k*j/32)) and -round(32768*sin(2*pi*k*j/32))
for k = 1..15 and j = 1..16, saturated to +-32767, and it matches to 1 LSB at
every one of its 480 entries.  So the sub-band stage's transform is a 32-point
real DFT evaluated directly, giving bins 0..15 - one per channel.

THE PITCH WINDOW
----------------
Vocoder_AnalyzeSpectrum also transforms 255 samples before pitch and voicing,
through PTR_DAT_00020E5C = 0x18000FA8 (file 0x064668).  Those 128 shorts sit
immediately before the 199-point Hamming at 0x180010A8, so the two are
contiguous the way the math primitives' block was.  It is monotone edge to
centre, peak 28193, but a 3-term least-squares fit leaves about 1000 LSB
against Hamming, Hann and Blackman alike, and there is an unexplained rate
break between entries 5 and 6.  It is extracted as measured rather than named.

All five of these tables are verbatim bytes, which is what this project does
with every table anyway; the analytic identifications above are checks on the
extraction, not the source of the values.

The block-length table is one uint16 per L (L = 9..56), holding four 4-bit
fields, least significant first, each biased by +2:

    Ji[i] = ((word >> (4*i)) & 0xF) + 2

which is why a byte-level search for it never found anything.  It reproduces
mbelib's AmbeLmprbl for all 48 rows.
"""
import sys

BASE = 0x0636C0           # file offset that SRAM 0x18000000 is loaded from
Q11 = 2048

TABLES = [
    # name              sram        count  cols
    ("ambe_prba24_q11", 0x18002090, 512,   3),
    ("ambe_prba58_q11", 0x18002c90, 128,   4),
    ("ambe_hoc_b5_q11", 0x18003090, 32,    4),
    ("ambe_hoc_b6_q11", 0x18003190, 16,    4),
    ("ambe_hoc_b7_q11", 0x18003210, 16,    4),
    ("ambe_hoc_b8_q11", 0x18003290, 8,     4),
    ("ambe_dg_q11",     0x180038b4, 32,    1),
]
# The ITU-style basic-operator coefficients, reached through each function's
# literal pool (see the module docstring).  All signed 16-bit Q15.
MATH_TABLES = [
    # name                  sram        count  cols  what it belongs to
    ("ambe_log2_coeff_q15", 0x18001600, 6,     6,
     "Math_Log2 0x0001903C / Math_Log2Scaled 0x000190C0"),
    ("ambe_pow2_coeff_q15", 0x1800160c, 6,     6,
     "Math_Pow2 0x000191C0 / Math_Pow2Scaled 0x00019280"),
    ("ambe_sqrt_coeff_q15", 0x18001618, 12,    6,
     "Math_Sqrt 0x00019364 / Math_SqrtScaled 0x000193E0, two sets of 6"),
    ("ambe_cos512_q15",     0x18001630, 512,   8,
     "Math_TableInterpLookup 0x00019000, cos(2*pi*i/512)"),
    ("ambe_fft_twiddle_q15", 0x18001a30, 512,  8,
     "Dsp_FftStageButterfly 0x00025160 via its pool at 0x00025220; "
     "256 complex pairs of exp(-j*2*pi*k/512), Q15, re then im"),
    ("ambe_fft_bitrev32",   0x180014e0, 25,    8,
     "Dsp_FftBitReverseScale 0x00025224 pool 0x00025480; count then "
     "delta-coded swap pairs, the N=32 bit-reversal permutation"),
    ("ambe_fft_bitrev128",  0x18001514, 113,   8,
     "Dsp_FftBitReverseScale 0x00025224 pool 0x000253D8; likewise for N=128"),
    ("ambe_anwin_q15",      0x180010a8, 100,   8,
     "Dsp_WindowAndComputeFft 0x00019B6C via Vocoder_ProcessFrame 0x00016E04's "
     "pool at 0x00016F38; half of a 199-point Hamming window"),
    ("ambe_pitchwin_q15",   0x18000fa8, 128,   8,
     "Vocoder_AnalyzeSpectrum 0x000205B8 via PTR_DAT_00020E5C; half of the "
     "255-point window it transforms before pitch and voicing.  Not a "
     "cosine-family window - recorded as measured"),
    ("ambe_subband_win_q15", 0x18001454, 16,   8,
     "Vocoder_AnalyzeSubbandSpectrum 0x00023AA8 via PTR 0x000240A8/0x00024920; "
     "half of the symmetric 32-tap window of the 16-channel filterbank"),
    ("ambe_subband_fir_q15", 0x18001474, 8,    8,
     "Vocoder_AnalyzeSubbandSpectrum's decimator, read at 0x00024EAA via PTR "
     "0x00025084; taps 0..3 mirrored to a symmetric 7, then one pad word"),
    ("ambe_subwin_q15",      0x18001484, 29,   8,
     "Vocoder_AnalyzeSpectrum's eight-band loop via PTR 0x00020B3C, walked "
     "downwards from 0x180014BC; half of the 58-tap sub-frame window"),
    ("ambe_subband_dft_q15", 0x180039ec, 480,  8,
     "Vocoder_SubbandSumDifference via PTR 0x0002B2D4; 15 rows of 16 cosines "
     "then 16 negated sines, the 32-point real DFT's bins 1..15"),
]

LMPRBL_SRAM = 0x18002030
LMPRBL_N = 48             # L = 9..56
VUV_SRAM = 0x18003628     # 128 x uint32, 2 bits per voicing band
VUV_N = 128


def s16(raw, off):
    v = raw[off] | (raw[off + 1] << 8)
    return v - 65536 if v >= 32768 else v


def emit(name, vals, per_line, comment):
    print("/* %s */" % comment)
    print("const short %s[%d] = {" % (name, len(vals)))
    for i in range(0, len(vals), per_line):
        row = ", ".join("%6d" % v for v in vals[i:i + per_line])
        print("    " + row + ("," if i + per_line < len(vals) else ""))
    print("};")
    print()


def main():
    raw = open(sys.argv[1], "rb").read()
    print("""
/*
 * ambe_tables_fw.c - AMBE+2 quantiser tables and the fixed-point math
 * primitives' coefficients, read out of the DM-32UV firmware.
 *
 * GENERATED by tools/extract_tables.py from
 *   firmware/DM32_L01_048_20250821.bin
 *   sha256 fda860febfcf1a234eed7fa73272112891074aac83746e4f8dfe224a2a700f8f
 * Do not edit by hand.  See that script for how each table was located and for
 * the firmware addresses; every table below is a verbatim copy of bytes in the
 * image, not a transcription of anyone else's values.
 *
 * The quantiser tables are signed 16-bit Q11 (divide by 2048); the math
 * primitives' coefficients are signed 16-bit Q15 (divide by 32768).
 *
 * SPDX-License-Identifier: ISC
 */
#include "ambe_tables.h"
""".strip())
    print()

    for name, sram, count, cols in TABLES:
        off = sram - 0x18000000 + BASE
        n = count * cols
        vals = [s16(raw, off + 2 * i) for i in range(n)]
        emit(name, vals, cols if cols > 1 else 8,
             "SRAM 0x%08X, file 0x%06X, %d x %d, Q11" % (sram, off, count, cols))

    for name, sram, count, per_line, owner in MATH_TABLES:
        off = sram - 0x18000000 + BASE
        vals = [s16(raw, off + 2 * i) for i in range(count)]
        emit(name, vals, per_line,
             "SRAM 0x%08X, file 0x%06X, %d x int16 Q15 - %s"
             % (sram, off, count, owner))

    # block lengths: unpack the nibbles into a plain [57][4], L = 0..56
    off = LMPRBL_SRAM - 0x18000000 + BASE
    ji = [0] * (57 * 4)
    for k in range(LMPRBL_N):
        w = raw[off + 2 * k] | (raw[off + 2 * k + 1] << 8)
        L = 9 + k
        for i in range(4):
            ji[L * 4 + i] = ((w >> (4 * i)) & 0xF) + 2
    # voicing patterns, exactly as the firmware stores and indexes them
    off = VUV_SRAM - 0x18000000 + BASE
    vuv = []
    for k in range(VUV_N):
        w = (raw[off + 4*k] | (raw[off + 4*k + 1] << 8) |
             (raw[off + 4*k + 2] << 16) | (raw[off + 4*k + 3] << 24))
        vuv.append(w)
    print("/* SRAM 0x%08X, file 0x%06X, %d x uint32.  Two bits per voicing band,"
          % (VUV_SRAM, off, VUV_N))
    print(" * most significant crumb first; the low bit of each crumb is the band's")
    print(" * voiced flag.  Vocoder_DecodeSpectralCodebookEntry 0x00022DB4 indexes this")
    print(" * with the b1 field left-justified to 7 bits, so for the 2450 mode's 5-bit")
    print(" * b1 the index is b1 << 2.  Crumb values 0, 1 and 2 all occur; only bit 0")
    print(" * is used here. */")
    print("const unsigned int ambe_vuv_packed[%d] = {" % VUV_N)
    for i in range(0, VUV_N, 4):
        row = ", ".join("0x%08Xu" % v for v in vuv[i:i+4])
        print("    " + row + ("," if i + 4 < VUV_N else ""))
    print("};")
    print()

    emit("ambe_lmprbl", ji, 4,
         "SRAM 0x%08X, file 0x%06X, 48 x uint16 nibble-packed, unpacked to [57][4];"
         " rows 0..8 are unused (L >= 9)" % (LMPRBL_SRAM, off))


if __name__ == "__main__":
    main()
