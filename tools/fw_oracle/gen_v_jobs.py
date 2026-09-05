#!/usr/bin/env python3
"""Oracle Vocoder_SynthesizeVoiced 0x0001DE10 by CALLING it, one case at a time.

The same two-step shape `gen_uv_jobs.py` uses for the unvoiced synthesiser, and
for the same reason: the inputs are channel state no caller can build from cold,
so `gen_synth_jobs.py`'s sequence capture has to learn them first, and only then
can they be poked back in and the function invoked directly.

Six arguments, read off Vocoder_SynthesizeFrame's marshalling at 0x00019EDC
rather than off the decompiler:

    Vocoder_SynthesizeVoiced(acc, n, pChannelState+0x18, pParams,
                             pPrevParams, nPitch)
                              r0  r1        r2             r3
                                              (sp,0x0)     (sp,0x4)

It reads nothing else.  The decompilation touches param_1..param_6 and one
flash literal (g_0001e12c, the voicing mask) and nothing besides, so a direct
call is faithful - the same argument gen_uv_jobs.py makes for the unvoiced side.

ONE POINTER HAS TO BE REWRITTEN.  pParams[0x40..0x41] is the address of the
per-harmonic voicing flags, which the harmonic spectrum builder dereferences.
I first assumed they were pState+0xad - Vocoder_SynthesizeFrame clears ctx+0x172
for 0x38 shorts at 0x00019DE0, and ctx+0x172 IS pState+0xad, so the arithmetic
was inviting.  It is wrong: measured over 345 captured calls the pointer holds
0x00057D70 from the current block and 0x00057D00 from the previous one, two
scratch arrays outside the channel state entirely.  Nothing in the state block
would have supplied them, and a job built on that assumption would have
synthesised from whatever happened to be at pState+0xad.

So the flags are a captured input like any other: gen_synth_jobs.py peeks both
arrays, the fixture carries the current frame's, and this job pokes it and
repoints pParams[0x40] at the copy.  The assertion that survived is the useful
one - the pointer is checked to lie inside the peeked window rather than being
trusted to be a constant.

    python3 tools/fw_oracle/gen_v_jobs.py /tmp/v.job [--limit N] [--stages]
    EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/v.job /tmp/v.out
    python3 tools/fw_oracle/gen_v_jobs.py --export /tmp/v.out \
            tests/fixtures/dm32_arc4_1.fwvoiced

What this is for is the audit as much as the fixture: feeding record i's
arguments back in has to reproduce record i's accumulator and state exactly,
and feeding record i-1's must not.  That is what says gen_synth_jobs.py paired
state with parameter block correctly, and it is the same check that caught the
pairing on the unvoiced side.

REQUIRES the C-SKY sleigh patched with `csky-muls.patch` AND `csky-mvcv.patch`
from the reverse-engineering project's `docs/patches/`.

SPDX-License-Identifier: ISC
"""
import os, struct, sys

REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))
import emu, vocoder_ctx as V

VOICED = 0x0001DE10

NS    = 0x50              # nCount, the only value the radio ever passes
NBLK  = 68
# Imported, not restated: these two set the fixture's column stride, and a copy
# that drifted from gen_synth_jobs.py's would misalign every record silently
# rather than failing.
from gen_synth_jobs import N_VST, N_VOI       # noqa: E402

# Scratch, above the stack, which grows down.  Laid out by size rather than on
# round boundaries, because these blocks are not all small: N_VST grew from 0x140
# to 0x22C once the spectrum buffer inside the state was found, at which point a
# layout spaced every 0x100 bytes had the state overlapping the blocks poked
# after it.  That does not fault - it quietly rewrites 44 shorts of the state
# with parameter-block bytes, and surfaces only as the state-after comparison
# failing at exactly the overlap.  So each address is derived from the end of the
# one before it, and cannot drift when a size changes again.
ACC   = 0x00051000                        # 80 int32, the accumulator - NOT zero
                                          # on entry: it already holds the
                                          # unvoiced contribution
STATE = ACC   + 100 * 4                   # N_VST shorts, pChannelState+0x18.
                                          # ACC is reserved 100 int32, not NS:
                                          # that is the width gen_synth_jobs.py
                                          # peeks, so anything the firmware's
                                          # buffer holds past the 80 samples has
                                          # room here rather than landing in the
                                          # state
BLK   = STATE + N_VST * 2                 # 68 shorts, the current block
PRV   = BLK   + NBLK * 2                  # 68 shorts, the previous one
VOI   = PRV   + NBLK * 2                  # N_VOI shorts, the voicing flags

# The stack frame, from the prologue: `push` of nine registers (0x24) then
# `subi sp,sp,0x36c`, under an entry sp of STACK_TOP-8 - EmuRun aligns sp down
# to 8 after making room for the two stack arguments.  The arithmetic closes on
# itself: the function loads param_5 with `ld.w r5,(sp,0x390)`, and
# STACK_TOP-0x398+0x390 is STACK_TOP-8, the first stack argument.
FRAME    = 0x398
SP_IN    = emu.STACK_TOP - FRAME
BUF_OFF  = 0x74           # `addi r5,sp,0x74` at 0x0001DF20 - local_31c, whose
                          # decompiler name agrees with STACK_TOP-0x398+0x74
BUF      = SP_IN + BUF_OFF
BUF_LEN  = 0x150 * 2      # `movi r2,0xa8 / lsli r2,r2,0x1` - 0x150 shorts

assert VOI + N_VOI * 2 < SP_IN, "the scratch blocks run into the stack frame"

# Two stage breaks were wanted here, on the Dsp_ calls whose arguments identify
# the two halves:
#   0x0001DFAE  Dsp_FillShortArray(local_31c, 0, 0x150) - the clear BETWEEN the
#               halves.  The accumulator carries the previous frame's decaying
#               harmonics and none of this frame's; local_31c is the buffer they
#               were synthesised into, about to be wiped.
#   0x0001E166  Dsp_CopyShortArray(pState, local_31c + n, 0xa8) - the history
#               store, where local_31c is this frame's buffer, complete.
# THEY ARE OFF BY DEFAULT, because neither is on the entry path.  Over 20 probe
# records the clear fired 20 times and the store 14: the function has two
# epilogues, 0x0001E180 and 0x0001E304, and a branch at 0x0001DF38 that leaves
# for 0x0001E462 - past the clear - so a record can reach the end having passed
# through neither.  The job file is a linear script: a break that does not fire
# leaves its `resume` to run a machine that has already returned, and from there
# the record count and the peeks are out of step with the fixture.  Deterministic
# stops matter more than staged ones, and unlike the unvoiced synthesiser this
# function does not offer both.
#
# `stages=True` still works for a subset known to take the main path, which is
# what a transcription would use once it is close enough for a half to be the
# unit of failure.  It is not what establishes the fixture.
BRK_CLEAR = 0x0001DFAE
BRK_STORE = 0x0001E166
NBRK      = 2


def _rows(path):
    out = []
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        out.append([int(x) for x in line.split()])
    return out


def _s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


# Column layout of the fixture gen_synth_jobs.export_voiced writes.
C_PITCH = 0
C_BLK   = 1
C_PRV   = C_BLK + NBLK
C_VST   = C_PRV + NBLK
C_VOI   = C_VST + N_VST
C_ACC   = C_VOI + N_VOI
N_HEAD  = C_ACC + NS               # everything up to and including acc-before


def gen(jobfile, src, limit=None, stages=False):
    rows = _rows(src)
    if limit:
        rows = rows[:limit]
    j = emu.Job()
    if stages:
        j.hook_break(BRK_CLEAR)
        j.hook_break(BRK_STORE)
    for i, r in enumerate(rows):
        blk = list(r[C_BLK:C_BLK + NBLK])
        blk[0x40] = VOI & 0xFFFF           # repoint at the poked copy of the
        blk[0x41] = (VOI >> 16) & 0xFFFF   # flags; the firmware's address is
                                           # scratch this job does not populate
        j.poke(ACC,   struct.pack("<%di" % NS, *r[C_ACC:C_ACC + NS]))
        j.poke(STATE, struct.pack("<%dh" % N_VST,
                                  *[_s16(x) for x in r[C_VST:C_VST + N_VST]]))
        j.poke(VOI,   struct.pack("<%dH" % N_VOI,
                                  *[x & 0xFFFF for x in r[C_VOI:C_VOI + N_VOI]]))
        j.poke(BLK,   struct.pack("<%dh" % NBLK, *[_s16(x) for x in blk]))
        j.poke(PRV,   struct.pack("<%dh" % NBLK,
                                  *[_s16(x) for x in r[C_PRV:C_PRV + NBLK]]))
        j.call(VOICED, ACC, NS, STATE, BLK, PRV, r[C_PITCH])
        if stages:
            j.getreg("sp"); j.peek("pbuf%d" % i, BUF, BUF_LEN)
            j.peek("pacc%d" % i, ACC, NS * 4); j.resume()
            j.getreg("sp"); j.peek("cbuf%d" % i, BUF, BUF_LEN); j.resume()
        j.peek("acc%d" % i, ACC, NS * 4)
        j.peek("st%d"  % i, STATE, N_VST * 2)
    j.write(jobfile)
    print("wrote %s: %d calls%s" % (jobfile, len(rows),
                                    "" if stages else " (no stage breaks)"))
    return len(rows)


def export(outfile, src, dest, stages=False):
    """Rewrite `src`'s output columns from a completed run.

    The stage breaks are on branches, not on the entry path, so a record that
    took a different route through the function produces a different number of
    stops.  That would silently pair one record's peek with another's, so `sp`
    is read at every stage stop and checked - a stop that is not inside this
    function's frame is a desynchronised run, not a datum.
    """
    rows = _rows(src)
    res = emu.parse(outfile)
    assert not res["faults"], res["faults"][:2]
    assert not res["errors"], res["errors"][:2]
    if stages:
        bad = [v for n, v in res["regs"] if n == "sp" and v != SP_IN]
        assert not bad, ("stage stop outside the frame: sp=%08x, expected %08x - "
                         "the run is desynchronised, rerun with stages=False"
                         % (bad[0], SP_IN))
        assert len(res["regs"]) == len(rows) * NBRK, (
            "%d stage stops for %d calls, expected %d: a break did not fire on "
            "every call" % (len(res["regs"]), len(rows), len(rows) * NBRK))
    n = 0
    with open(dest, "w") as fh:
        fh.write("# Vocoder_SynthesizeVoiced 0x0001DE10, called directly under the\n"
                 "# p-code emulator over the inputs Vocoder_TxTask produced for\n"
                 "# tests/fixtures/dm32_arc4_1.frames.  tools/fw_oracle/gen_v_jobs.py.\n"
                 "# per record: pitch, %d shorts of the current parameter block, %d of\n"
                 "#   the previous, %d shorts of state before, the %d voicing flags,\n"
                 "#   the %d int32 accumulator before (the unvoiced contribution alone),\n"
                 % (NBLK, NBLK, N_VST, N_VOI, NS))
        if stages:
            fh.write("#   the %d-short buffer the PREVIOUS frame's harmonics were\n"
                     "#   synthesised into and the accumulator once they had been added,\n"
                     "#   the %d-short buffer of THIS frame's,\n" % (0x150, 0x150))
        fh.write("#   then the %d int32 accumulator after and %d shorts of state after\n"
                 % (NS, N_VST))
        for i, r in enumerate(rows):
            cols = [" ".join(map(str, r[:N_HEAD]))]
            if stages:
                cols.append(" ".join(map(str, struct.unpack("<%dh" % 0x150,
                                         res["peek"]["pbuf%d" % i][0]))))
                cols.append(" ".join(map(str, struct.unpack("<%di" % NS,
                                         res["peek"]["pacc%d" % i][0]))))
                cols.append(" ".join(map(str, struct.unpack("<%dh" % 0x150,
                                         res["peek"]["cbuf%d" % i][0]))))
            cols.append(" ".join(map(str, struct.unpack("<%di" % NS,
                                     res["peek"]["acc%d" % i][0]))))
            cols.append(" ".join(map(str, struct.unpack("<%dh" % N_VST,
                                     res["peek"]["st%d" % i][0]))))
            fh.write(" ".join(cols) + "\n")
            n += 1
    print("%s: %d calls" % (dest, n))
    return n


def audit(outfile, src, shift=0):
    """Does a direct call reproduce what the task run recorded?

    With shift=0 it has to, for every record: same arguments, same machine, same
    answer.  With shift=1 record i's answer is held against record i+1's, and it
    has to NOT match - otherwise the check is a tautology, and would pass on a
    capture that had paired state with the wrong frame all along.  Both questions
    come out of the same run: the shift is applied here, not in the job.
    """
    rows = _rows(src)
    res = emu.parse(outfile)
    ok_acc = ok_st = n = 0
    for i in range(len(rows)):
        if "acc%d" % i not in res["peek"]:
            break
        # under a shift the job's record i ran the arguments of fixture row i,
        # and what it must be compared against is row i+shift's outputs - the
        # frame those arguments did NOT come from
        if i + shift >= len(rows):
            break
        r = rows[i + shift]
        want_acc = r[N_HEAD:N_HEAD + NS]
        want_st  = r[N_HEAD + NS:N_HEAD + NS + N_VST]
        got_acc = list(struct.unpack("<%di" % NS, res["peek"]["acc%d" % i][0]))
        got_st  = list(struct.unpack("<%dh" % N_VST, res["peek"]["st%d" % i][0]))
        ok_acc += got_acc == want_acc
        ok_st  += got_st == [_s16(x) for x in want_st]
        n += 1
    print("shift %d: accumulator %d/%d, state %d/%d" % (shift, ok_acc, n, ok_st, n))
    return ok_acc, ok_st, n


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "--export":
        export(a[1], a[2], a[2])
    elif a[0] == "--audit":
        # the shift lives entirely in the audit: one run answers both questions
        audit(a[1], a[2], int(a[3]) if len(a) > 3 else 0)
    else:
        limit = None
        if "--limit" in a:
            limit = int(a[a.index("--limit") + 1])
        gen(a[0], a[1] if len(a) > 1 and not a[1].startswith("--")
            else "tests/fixtures/dm32_arc4_1.fwvoiced", limit, "--stages" in a)
