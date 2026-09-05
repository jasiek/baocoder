# The encode path, against the firmware's own encoder

The decode path has exact parity: payload, classification, `L`, `f0` and every
voicing decision, with the spectral envelope inside 0.021 dB
(`tests/test_firmware.c`). **The encode path has none of that**, and this is
what it has instead — measured against the radio rather than against a proxy.

## The oracle

`Vocoder_RxTask 0x0002E5A0` is the transmitter. Its entry point is

```c
Vocoder_ProcessFrameFec(pDest, param_2, pSamples, nFrameSize, wFlags,
                        bQuantise, param_7, pCtx)
    -> Vocoder_ProcessFrame(pCtx, pSamples, nFrameSize, wFlags, bQuantise)
    -> if (bQuantise & 1) Vocoder_EncodeFrameParameters(pDest, ...)
```

A 20 ms frame is two 80-sample calls, `bQuantise` 0 then 1.  The arguments are
not guessed: `Vocoder_RxTask` passes `0x50, 0x800, 1` and `0x50, 0x800, 0`, and
`tools/fw_oracle/gen_encode_jobs.py` passes the same.

The input is `dm32_arc4_1.fwpcm`, the radio's own decoded audio for a real
capture, so both encoders see identical samples.

## What the firmware exposes, and what it does not

The analysis parameters live at **`RX_CTX + 0xABC`**, in the same
`Vocoder_CopyFrameParamsWithReset 0x00019CBC` layout the decode side uses:
`+0x00` class, `+0x02` the `0xFF` marker, `+0x04` `L`, `+0x0C` `f0` Q19,
`+0x10` the log2 envelope at Q11.  That block was found by scanning a 0x2000
window for the signature, not by reading the decompiler —
`Vocoder_AnalyzeSpectrum` is handed `pCtx+0x938` and `pCtx+0xB50`, and it is
neither of those.

Three things are **not** available, each checked rather than assumed:

* **The word at `+0x08` is not the voicing field.** On the decode path it is,
  and `test_firmware` verifies that decoding on 71 977 decisions.  Here its
  bits agree with the transmitted `b1`'s codebook expansion only 57% of the
  time — barely above chance — and it grows monotonically across frames in a
  way that does not track `L`.
* **No offset in a 0x2000 window holds the pitch `b0` decodes to.**
* **Nor does any offset hold `b1`'s codebook expansion.**

The encoder searches for indices without ever materialising what they mean.
So the quantiser can only be scored index against index, and voicing can only
be compared against the transmitted `b1`.

## The alignment, which had to be measured first

The firmware's analysis of frame *i* covers audio **240 samples — one and a
half frames — earlier**, which is what `Vocoder_ProcessFrame 0x00016E04`'s
258-sample rolling window implies.  Scored at lag zero instead, this decoder
looks far worse than it is:

| alignment | L exact | pitch ±6% | octave | envelope |
|---|---|---|---|---|
| lag 0 | 31% | 74% | 7% | 5.82 dB |
| **240 samples** | **60%** | **85%** | **4%** | **2.75 dB** |

## Stage A — the analyser

`src/ambe_analysis.c` is the one stage that is not a transcription: its FFT
front end is the radio's, its pitch and voicing decisions are not.  Over the
239 voice frames, at the alignment above:

| | |
|---|---|
| harmonic count `L` exact | 60% |
| pitch within ±6% | 85% |
| octave errors | 4% |
| envelope shape, mean removed | 2.75 dB |
| voicing agreement | 43% |

**Voicing is the gap, and it is a bias rather than noise.** The radio calls
65% of harmonics voiced; this analyser calls 15%.  Agreement is *below* the
trivial always-voiced answer.  But precision is high — most of what it calls
voiced is — and recall is about a fifth, so the measure carries signal and the
threshold is what is wrong.  That is a much cheaper thing to fix than a
detector that does not work.

`test_encode_voicing` scores the same decision at 72% against a 67% baseline
on *this decoder's own* decoded audio.  Both are correct: the estimator does
measurably worse on the radio's audio than on baocoder's.  The codebook search
also masks the raw skew, lifting band agreement to 67% against a 63% baseline,
because most `b1` entries are mostly-voiced — only the raw per-harmonic
comparison shows the 15%-against-65%.

## Stage B — the quantiser

`src/ambe_encode_params.c` *is* meant to be the firmware's.  Handing it the
radio's own analysis parameters — and, since the analysis block has no usable
voicing, the radio's own `b1` — isolates it from the analyser:

| index | exact |
|---|---|
| `b0` pitch | 12% (30% within one step) |
| `b1` voicing | 5% |
| `b2` gain | 0% |
| `b3` PRBA24 | 0% |
| `b4` PRBA58 | 1% |
| `b5`–`b8` HOC | 1–22% |

Two things are known about this, and one is the obvious next lever:

* **`b0` is not a direct quantisation of the analysis pitch.** Line 150 of
  `ambe_encode_params.c` is `b0 = quantise_b0(cur->f0)`, nearest index.  The
  firmware's is not: over runs of frames where the analysis `f0` drifts several
  percent, its `b0` stays constant.  It is searching against some criterion,
  not rounding.
* **`b2` is wrong by a constant sign.** Our gain index is *below* the
  firmware's on every one of the 239 frames, median 20 steps, range −30..−6 on
  a 5-bit field — pinned near the floor.  `b3`/`b4` are the PRBA vectors
  computed after gain normalisation, so one wrong gain invalidates everything
  downstream of it.  That single offset, not eight independent disagreements,
  is what the table above is mostly showing.

Closing `b2` is the first thing to try, and it needs
`Vocoder_EncodeFrameParameters 0x0001994C`'s gain path read — which has not
been done.

## Regenerating

```sh
REVENG=../baofeng-dm32uv-reveng
python3 tools/fw_oracle/gen_encode_jobs.py /tmp/enc.job \
        tests/fixtures/dm32_arc4_1.fwpcm 360
EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/enc.job /tmp/dm32_arc4_1.out
python3 tools/fw_oracle/export_encode.py /tmp tests/fixtures dm32_arc4_1
```

SPDX-License-Identifier: ISC
