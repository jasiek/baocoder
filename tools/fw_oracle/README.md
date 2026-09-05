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

`docs/patches/csky-mvcv.patch` corrects the 16-bit `mvcv` constructor, which
produced 0xFE/0xFF instead of 0/1 and so made every branch on a condition the
compiler had moved into a register go the same way. It is not optional: without
it `Vocoder_SynthesizeUnvoiced` takes the wrong one of its two gain paths on
every frame.

Regenerating `tests/fixtures/dm32_arc4_1.frames` with and without it says which
fixtures the defect touched. `.fwparms`, `.fwenv` and `.fwamps` come out
**identical** - the parameter path never used it. `.fwpcm` does not: 31.4% of
57 600 samples differ, worst 57 LSB against a 1 984 peak, 35.0 dB SNR.

`*.fwunvoiced` is regenerated. **`*.fwpcm` and everything derived from it -
`*.fwencbits`, `*.fwanalysis`, `*.fwsynth`, `*.fwpostfilter`, `*.fwifft` - still
predate the fix.** Those drive tolerance tests rather than bit-exactness
assertions, so they are stale rather than wrong-and-failing, but the
regeneration is outstanding work and the encoder fixtures have to be re-run
*after* the PCM they take as input, not alongside it.

## Calling one function instead of watching it

`gen_uv_jobs.py` is a different shape of job again: it pokes the arguments of
`Vocoder_SynthesizeUnvoiced 0x0001AFE0` into scratch and calls it, one case per
record, instead of catching it inside a running task. That is only possible
once the *inputs* are known - they are channel state, and `gen_synth_jobs.py`'s
sequence capture is what learns them - but once they are, it is better in three
ways: a record cannot pair one call's state with another call's parameter
block, 103 calls take 27 seconds rather than ten minutes, and the inputs can be
varied, which is the only way to reach the frame-class-2 gain path this corpus
never contains.

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
