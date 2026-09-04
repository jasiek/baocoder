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

## The payload permutation

The firmware's 49-bit buffer is in **field order** - `b0`(7) `b1`(5) `b2`(5)
`b3`(9) `b4`(7) `b5`(5) `b6`(4) `b7`(4) `b8`(3), contiguous - where `ambe_d[]`
is mbelib's scattered order. The map between them is not exported as a fixture
because nothing in the library needs it; it was measured by pushing 49 one-hot
payloads through the firmware, each of which produced exactly one output bit,
and it is what identified the `b3`/`b4` assignment in `src/ambe_params.c`.

SPDX-License-Identifier: ISC
