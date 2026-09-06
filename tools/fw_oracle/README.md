# Regenerating the firmware fixtures

`tests/fixtures/*.fwparms`, `*.fwamps` and `*.fwpcm` are not another decoder's
opinion: they are what the DM-32UV's own AMBE decoder produces when the firmware
is *executed*. `tests/test_firmware.c` is the parity test that compares against
them, so this is the one part of the corpus that cannot be regenerated from this
repository alone.

It needs [`baofeng-dm32uv-reveng`](https://github.com/…) checked out beside this
one, with its Ghidra install and the C-SKY sleigh patches in
`docs/patches/`, because the emulator lives there. The whole corpus is about
forty minutes of emulation across four parallel workers.

```sh
REVENG=../baofeng-dm32uv-reveng

# once, and after any sleigh change
$REVENG/tools/emu/run.sh init

# 49-bit payloads -> on-air frames the firmware's FEC will accept
make tools/fw_oracle/bits_to_frames
for c in dm32_arc4_1 dm32_arc4_2 dm32_aes128_1 dm32_aes128_2 \
         dm32_aes256_1 dm32_aes256_2; do
    ./tools/fw_oracle/bits_to_frames tests/fixtures/$c.ambe49 > /tmp/$c.frames
    python3 tools/fw_oracle/gen_jobs.py /tmp/$c.job /tmp/$c.frames \
            $(wc -l < /tmp/$c.frames)
    EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/$c.job /tmp/$c.out
done

python3 tools/fw_oracle/export.py /tmp tests/fixtures
```

## The sleigh has to be patched first

`csky-mvcv.patch`, in the reverse-engineering project's `docs/patches/`
alongside the other C-SKY sleigh fixes, corrects the 16-bit `mvcv`
constructor, which produced 0xFE/0xFF instead of 0/1 and so made every branch
on a condition the compiler had moved into a register go the same way. It is
not optional: without it `Vocoder_SynthesizeUnvoiced` takes the wrong one of
its two gain paths on every frame.

Regenerating `dm32_arc4_1` with and without it says which fixtures the defect
touched, and the answer is a clean split down the middle of the vocoder:

| fixture | before vs after the patch |
|---|---|
| `.fwparms`, `.fwenv`, `.fwamps` | **byte-identical** - the parameter path never used `mvcv` |
| `.fwpcm` | 31.4% of 57 600 samples differ, worst 57 LSB against a 1 984 peak, **35.0 dB SNR** |
| `.fwanalysis` | 97 of 360 frames differ |
| `.fwaenv` | 352 of 360 |
| `.fwencbits` | 342 of 360 |

Everything in that table has been regenerated, in that order, because the order
is load-bearing: the encoder oracle takes `.fwpcm` as its *input*, so re-running
it alongside the decoder rather than after it would pair new bits with old
audio. `.fwsynth`, `.fwpostfilter` and `.fwunvoiced` were re-captured with it.

What moved as a result, all of it still inside the thresholds the tests assert:
`test_synth`'s worst-case band correlation 0.805 -> **0.831** and mbelib's on the
same reference 0.968 -> **0.969** (the means did not move); `test_firmware_encode`
octave errors 4% -> **2%**, envelope 2.74 -> **2.70 dB**, `L` exact 60% -> **59%**,
pitch within 6% 85% -> **83%**. The corrected reference is very slightly easier
to match, which is the direction a more-correct reference should move in.

**`.fwfft` and `.fwifft` are the exception and are deliberately not
regenerated.** They are input/output pairs for `Dsp_FftForward` and
`Dsp_FftInverse`, and `test_fft_firmware` asks nothing of them but that the
transform reproduce the pair - which it does, bit for bit, patched or not. Their
inputs are real buffers from a real run; they are simply not index-aligned with
anything else in the corpus, so do not try to read `.fwifft` record *i* as the
spectrum `.fwunvoiced` record *i* hands the inverse transform. It is not, and an
afternoon went into learning that.

## Calling one function instead of watching it

`gen_uv_jobs.py` is a different shape of job again: it pokes the arguments of
`Vocoder_SynthesizeUnvoiced 0x0001AFE0` into scratch and calls it, one case per
record, instead of catching it inside a running task. That is only possible
once the *inputs* are known - they are channel state, and `gen_synth_jobs.py`'s
sequence capture is what learns them - but once they are, it is better in three
ways: 103 calls take 27 seconds rather than ten minutes, the inputs can be
varied - the only way to reach the frame-class-2 gain path this corpus never
contains - and it audits the capture it was built from. Feeding record *i*'s
inputs back in reproduces record *i*'s outputs exactly, 103 of 103, which is
what establishes that `gen_synth_jobs.py` paired state with parameter block
correctly; taking the block from record *i-6* instead reproduces 0 of 97.

```sh
python3 tools/fw_oracle/gen_uv_jobs.py /tmp/uv.job
EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/uv.job /tmp/uv.out
python3 tools/fw_oracle/gen_uv_jobs.py --export /tmp/uv.out \
        tests/fixtures/dm32_arc4_1.fwunvoiced
```

The job breaks twice inside each call, at the two transforms, so the fixture
carries the windowed noise going in and the shaped spectrum coming out as well
as the samples - a failure lands on a stage rather than on 713 instructions.

## What the job does

`gen_jobs.py` drives `Vocoder_TxTask 0x0002EC30` itself rather than
reconstructing the receive sequence around `Vocoder_ConfigureFrame`, which is
what `tools/emu/oracle_dectask.py` in the reveng project established: the task
calls `ConfigureFrame` twice per AMBE frame, into different slots of a
160-sample ring, and driving only one half leaves a history index advancing
without its counterpart resetting it. The harness supplies the RTOS stubs, the
audio ISR's ring drain, and the nine on-air bytes per frame; everything else is
the firmware's.

At each wake it peeks the payload buffer, the ring, and the parameter block at
`pCtx+1000`, whose layout is `Vocoder_CopyFrameParamsWithReset 0x00019CBC`'s.

## Sweeping a leaf instead of capturing one

`gen_basop_jobs.py` is the cheapest shape of job in here, and the one to reach
for first when a transcription is about to depend on a primitive.
`Math_FloatAdd 0x00018DD8` and `Math_FloatDivExponent 0x00018EF4` are leaves -
no state, no memory but the one exponent they write - so they need no capture to
learn their inputs. They can just be swept.

```sh
python3 tools/fw_oracle/gen_basop_jobs.py /tmp/basop.job
EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/basop.job /tmp/basop.out
python3 tools/fw_oracle/gen_basop_jobs.py --export /tmp/basop.out \
        tests/fixtures/basop_float.fw
```

3377 cases, edges first, and the edges are the point: an exponent difference of
exactly 31 makes the smaller operand shift by 32, which C leaves undefined and
the machine renders as everything shifted out. A transcription using C's own
`>>` - which on an x86 or ARM host is a shift masked to five bits, so no shift at
all - disagrees with the radio on **257 of the 3377**, and on none of the cases a
sweep that never reaches a difference of 31 would contain.

Doing this before writing anything that calls them is the whole economy of it:
`Vocoder_ComputeHarmonicGains 0x0001D71C` calls the pair four times per harmonic,
so one wrong bit is forty divergences in a single frame, found in 2378 bytes of
synthesiser rather than in thirty lines of `ambe_basop.c`.

## Sweeping the helpers too

`gen_vhelper_jobs.py` does the same for the functions inside
`Vocoder_SynthesizeVoiced`, so each can be settled before the one above it is
written. `Vocoder_ComputeHarmonicGains 0x0001D71C` is the innermost and reads
nothing but its seven arguments, so it sweeps as cleanly as a leaf.

```sh
python3 tools/fw_oracle/gen_vhelper_jobs.py /tmp/vh.job
EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/vh.job /tmp/vh.out
python3 tools/fw_oracle/gen_vhelper_jobs.py --export /tmp/vh.out \
        tests/fixtures/voiced_gains.fw
```

Sixteen of its 1600 cases are absent from the fixture and the export says so
rather than asserting: the function is not total. Its divisor is the
denominator's mantissa, and arguments where that normalises to zero make `divs`
trap - a zero sample count, a zero previous pitch with no pitch movement, and
`prev_pitch = -0x4000` with `delta = -1`, where the two terms under the root
cancel. A transcription cannot be asked what the radio does on an input the
radio faults on.

The output buffers are poked `0xEEEE` before each call, so a short the firmware
did not write reads as 61166 rather than as a plausible zero. That is what
catches a transcription writing the right values into the wrong slots. The
envelope interpolator's accumulator is poked with a ramp for the same reason:
that function *adds*, and a transcription that assigned would pass against a
zeroed buffer.

`Vocoder_InterpolateSpectralEnvelope 0x0001D9F0` needs one more restraint. It
does not range-check its destination: the rising taper is 0x10 long and runs
from the start position unconditionally - only the *end* is clamped, to 0xA6 -
so a start at 0xA6 writes to 0xB6, past the 0xA8-int accumulator. Under the
emulator that lands in scratch and the peek never sees it, which is a fixture
recording the firmware scribbling. The caller cannot produce such a start, so
the sweep filters them out by re-deriving the start index in Python, and the
test asserts that no case wrote past 0xA8.

## The frame layer above it

`gen_synth_jobs.export_frame` writes the oracle for
`Vocoder_SynthesizeFrame 0x00019DB8`, the layer between the decoded parameters
and the two synthesisers. `export` has always written that function's block and
its 80 samples, which is enough to *measure* it and not enough to *replay* it:
it reads and writes channel state at ctx+0x18, +0x172, +0x470, +0x4f8, +0x648,
+0x7ba and +0x7be, and a transcription handed only the block reproduces none of
it. So the capture peeks the whole 0x800-byte context at every stop - the whole
of it rather than those seven fields, because the list came from reading the
decompilation and an eighth would otherwise be found the hard way.

The fixture is not the whole of it. Carrying two copies of the context per call
was 5.3 MB and could not be trimmed until something knew which bytes mattered;
measuring the capture answers that, and the answer is that **nothing outside
this function writes any of them between calls** except the bypass word at
+0x7ba (15 of 616 gaps) and the tail of the voiced-state window. So the fixture
is a SEQUENCE: the state once, at the first call, then per call the inputs, the
80 samples, the block left at ctx+0x470 and the three scalars. 543 KB, and a
transcription that gets any bit of any stage wrong diverges and never recovers.

The voiced-state tail is worth its own line. That window is 0x22C shorts,
chosen by the capture to reach the next structure the memory map knows about;
what changes between calls is exactly `[0x1E8..0x227]` and nothing below it ever
does. So the voiced synthesiser's state proper ends at `0x1E8` - which is where
reading `Vocoder_SynthesizeHarmonicSpectrum 0x0001D4C8` put it, the harmonic
spectrum at `[0xE6]` running 0x100 shorts - and the rest of the window belongs
to something else. It is carried anyway; if the synthesiser ever read it, the
sequence would say so.

```sh
python3 tools/fw_oracle/gen_synth_jobs.py /tmp/sf.job /tmp/dm32_arc4_1.frames 360
EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/sf.job /tmp/sf.out
python3 -c "import sys; sys.path.insert(0,'tools/fw_oracle'); import gen_synth_jobs as G; \
            G.export_frame('/tmp/sf.out','tests/fixtures/dm32_arc4_1.fwframe')"
```

`tests/test_frame.c` consumes it, and mutating the layer says what the sequence
reaches. Moving the high-frequency tilt's corner, its 5/3 factor or its step
leaves **0 of 617**; flattening the block onto the smallest per-harmonic
exponent instead of the largest leaves 1; halving the ramp's reciprocal 227;
writing the Hilbert transform one short off in the channel state 192. Four
things it does not reach, and they are recorded as untested rather than
presented otherwise: the 0x3333 clamp on the smoothed pitch, which no frame of
this capture exceeds; the previous block's voicing flags, which the stock code
builds for `Vocoder_SynthesizeVoiced`'s `pPrev[0x40]` and `src/ambe_voiced.c`
does not read; the copy into the silence slot at ctx+0x4f8, which nothing reads
back; and the zero fill of ctx+0x172 on a non-voice frame.

## Building the frames a corpus does not contain

`gen_tone_jobs.py` calls `Vocoder_SynthesizeFrame 0x00019DB8` directly on tone
frames it constructs, because `pFrameParams[1]` is `0xFF` on all 617 calls of
the capture - every frame of the corpus is speech or silence - so the
function's tone branch has no natural coverage whatsoever.

```sh
python3 tools/fw_oracle/gen_tone_jobs.py /tmp/tone.job
EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/tone.job /tmp/tone.out
python3 tools/fw_oracle/gen_tone_jobs.py --export /tmp/tone.out \
        tests/fixtures/frame_tone.fw
```

The channel state comes out of `tests/fixtures/dm32_arc4_1.fwframe`'s own `S`
line, so the job is reproducible from this repository plus the emulator rather
than from a scratch capture. Two breaks, both on the unconditional path so
neither can fail to fire: `0x00019E26`, after the two
`Vocoder_ResetFrameBuffer` calls and before the tone test, which is the block
**as the branch sees it** - the preprocessing above has already rewritten it -
and `0x00019EAA`, where every path joins to synthesise.

And the job stops at the second one rather than resuming to the return. That is
not tidiness: run these frames on into the synthesiser and 467 of 616 die on a
divide by zero at `0x00018F14`, the same fault the voiced capture hit, because
they are built for the tone branch and not for the synthesiser below it.
`doCall` resets `sp`, `lr` and `pc`, so the next case simply starts.

Mutation says the fixture reaches the branch: the notch factor `0x51E` leaves
842 of 1 044, notching the tone bins as well 450, the DCS merge range 1 030,
the walk-up loop's step 906 and its start 930, and handing the index over
without its `-1` 686. Two things it does not separate, and both are recorded
rather than presented otherwise: whether the `abs` is a true negation or a
saturating one, which differs only at `-0x8000` and the indices are under
`0x38`; and, in the first version of the case set, the early path's two gates -
`|params[3] - prev[3]| < 2` widened to 3 and `(prev[3] + 1) * 16` halved both
left it at 616 of 616, because no combination of the grid's indices differs by
exactly two or puts a code in the halved window. Cases were added for both.

## The voiced synthesiser

`Vocoder_SynthesizeVoiced 0x0001DE10` is the one stage of the codec with no
transcription, and `tests/fixtures/dm32_arc4_1.fwvoiced` is the oracle for
writing one: 617 calls, every one the task run made over
`tests/fixtures/dm32_arc4_1.frames`, with all six arguments, the accumulator on
both sides and the channel state before and after.

```sh
# the inputs, from the sequence capture (about ten minutes)
python3 tools/fw_oracle/gen_synth_jobs.py /tmp/synth.job /tmp/dm32_arc4_1.frames 360
EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/synth.job /tmp/synth.out
python3 -c "import sys; sys.path.insert(0,'tools/fw_oracle'); import gen_synth_jobs as G; \
            G.export_voiced('/tmp/synth.out','tests/fixtures/dm32_arc4_1.fwvoiced')"

# the audit: call the function directly on those inputs (about a minute)
python3 tools/fw_oracle/gen_v_jobs.py /tmp/v.job tests/fixtures/dm32_arc4_1.fwvoiced
EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/v.job /tmp/v.out
python3 tools/fw_oracle/gen_v_jobs.py --audit /tmp/v.out tests/fixtures/dm32_arc4_1.fwvoiced 0
python3 tools/fw_oracle/gen_v_jobs.py --audit /tmp/v.out tests/fixtures/dm32_arc4_1.fwvoiced 1
```

| audit | accumulator | state |
|---|--:|--:|
| record *i*'s arguments against record *i*'s answer | **617 / 617** | **617 / 617** |
| against record *i+1*'s | 0 / 616 | 0 / 616 |
| against record *i+2*'s | 0 / 615 | 0 / 615 |

Once the transcription existed it reproduced all 617 on both the accumulator and
all 556 shorts of state - but the fixture cannot reach everything. Mutating
either octave-repair branch out of `src/ambe_voiced.c` leaves the whole-function
test at 617 of 617: with both frames voiced, a previous pitch between 0.4 and 0.6
of this one halves this one, and no frame of this capture does that. So
`--octave` builds the cases instead, taking real records and rewriting the two
pitch fields into the band:

```sh
python3 tools/fw_oracle/gen_v_jobs.py --octave /tmp/oct.job \
        tests/fixtures/dm32_arc4_1.fwvoiced
EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/oct.job /tmp/oct.out
python3 tools/fw_oracle/gen_v_jobs.py --export-octave /tmp/oct.out \
        tests/fixtures/dm32_arc4_1.fwvoiced tests/fixtures/voiced_octave.fw
```

240 constructed calls, and they earned their keep immediately: the halving path
takes `-phase >> 1` on a signed int, an arithmetic shift, and the transcription
had it as a logical one. Real speech never reaches that line, so the 617-call
fixture was green with the bug in place.

The second line is the point. The first would pass on a capture that had paired
state with the wrong frame from the beginning; the second says the pairing
carries information, the same check that established the unvoiced capture.

### Three things the arguments cost to get right, and all three failed loudly

**The state does not end where its named fields do.** The function's own loads
off `r11` stop at byte `0x1ca`, and it hands helpers pointers at shorts `0xab`,
`0xad`, `0xe5` and `0xe6`. A first window of `0x140` shorts was sized on the
assumption that `[0xe6]` was another `0x38`-long array like `[0xad]` - which is
what `Vocoder_SynthesizeFrame` clears at ctx+0x172, and ctx+0x172 *is*
pState+0xad, so the reasoning looked closed. It is not: `[0xe6]` is the harmonic
spectrum, `Vocoder_SynthesizeHarmonicSpectrum 0x0001D4C8` clears it `0x80<<1`
shorts at a time before handing it to `Dsp_FftInverse`, so the state runs to at
least `0xe6+0x100 = 0x1e6`. The window is now the whole span to the next
structure the map knows about, `0x22C` shorts.

**The voicing flags are not in the state either.** `pParams[0x40..0x41]` points
at them and the spectrum builder dereferences them. Measured across the capture
the pointer holds exactly one value from the current block, `0x00057D70`, and
one from the previous, `0x00057D00` - two adjacent scratch arrays `0x70` bytes
apart, which is the `0x38` shorts `Vocoder_BuildFrameResetPattern` fills. They
are captured and repointed at the poked copy, and the pointer is checked to lie
inside the peeked window rather than assumed to be that constant.

**Truncated state and overlapping scratch fail identically.** Both produced 337
faults out of 617, every one a divide by zero at `0x00018F14` - a zero divisor
reached through `Vocoder_InterpolateSpectralEnvelope`. The first was the state
window ending inside the spectrum buffer; the second was a scratch layout on
`0x100` boundaries that, once the window grew to `0x458` bytes, had the state
overlapping the parameter block poked after it. Same symptom, same count,
different cause. The addresses are now derived from the sizes, so a size that
changes again cannot silently re-create it.

### No stage breaks, and why

The unvoiced job breaks twice inside the call so a failure lands on a stage. The
two equivalent boundaries here - `0x0001DFAE`, the clear between the previous
frame's harmonics and this frame's, and `0x0001E166`, the history store - are
both on branches. Over 20 probe records the first fired 20 times and the second
14: the function has two epilogues, `0x0001E180` and `0x0001E304`, and a branch
at `0x0001DF38` that leaves for `0x0001E462`, past the clear. A job file is a
linear script: a break that does not fire leaves its `resume` to run a machine
that has already returned, and every peek after it belongs to the wrong record.
`--stages` still works on a subset known to take the main path; it is not what
establishes the fixture.

### One peek, no new break

Everything the voiced call is handed is read at `BRK_AFTER_UV 0x00019ED2`,
twelve instructions before the call, because those twelve are loads, moves and
two stack stores and none of them writes the state, the blocks or the pitch.
That matters more than it looks: `gen_synth_jobs.py`'s frame feed is keyed to
the number of stops per wake, so a new break would have shifted the sequencing
of a job three other fixtures come out of. A peek cannot.

## Breaking inside the interpolator

`resample_probe.py` is a different kind of job: it stops the emulator *inside*
`Vocoder_ResampleSpectralEnvelope 0x00026A84` and reads the two scratch arrays
the interpolation is built from, which is the only way to see which of its three
pitch branches fired.

```sh
python3 tools/fw_oracle/resample_probe.py /tmp/probe.job /tmp/frames.txt 360
EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/probe.job /tmp/probe.out
python3 tools/fw_oracle/resample_probe.py --check /tmp/probe.out
```

The check re-derives the pitch selection, both resamplings and the mix from the
peeked bytes and reports how many of each match the firmware exactly.  It also
breaks at `Vocoder_SynthesizeFrame 0x00019DB8` and reads `r0`, which is what
established that the interpolated block is the synthesiser's input for the first
80-sample half of every frame and `PARAMS+0x000` for the second.  It is not
part of `make test`: it needs the emulator, and it produces a measurement rather
than an assertion about this library.  Its frame feed drifts from the corpus -
see the module docstring - so its output is not comparable with the `.fw*`
fixtures frame by frame, and every check it makes is within a single stop.

## The payload permutation

The firmware's 49-bit buffer is in **field order** - `b0`(7) `b1`(5) `b2`(5)
`b3`(9) `b4`(7) `b5`(5) `b6`(4) `b7`(4) `b8`(3), contiguous - where `ambe_d[]`
is mbelib's scattered order. The map between them is not exported as a fixture
because nothing in the library needs it; it was measured by pushing 49 one-hot
payloads through the firmware, each of which produced exactly one output bit,
and it is what identified the `b3`/`b4` assignment in `src/ambe_params.c`.

SPDX-License-Identifier: ISC
