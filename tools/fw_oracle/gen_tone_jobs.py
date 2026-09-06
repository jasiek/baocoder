#!/usr/bin/env python3
"""Build tone frames and run Vocoder_SynthesizeFrame 0x00019DB8 on them.

pFrameParams[1] is 0xFF on all 617 calls of the capture - every frame of the
corpus is speech or silence - so the function's tone branch has no natural
coverage at all.  This constructs it, the way tools/fw_oracle/gen_v_jobs.py
--octave constructs the voiced synthesiser's octave repair.

The channel state comes out of tests/fixtures/dm32_arc4_1.fwframe's own `S`
line, so the job is reproducible from this repository plus the emulator, and so
the synthesis chain below the tone branch is handed a state a real run produced
rather than a zeroed one it would divide by.

Two breaks, and both are on the unconditional path so neither can fail to fire:

  0x00019E26  after the two Vocoder_ResetFrameBuffer calls and before the tone
              test, which is the block AS THE TONE BRANCH SEES IT - the
              preprocessing above has already rewritten it
  0x00019EAA  where every path joins to synthesise, so the block there is the
              tone branch's output and nothing else's - and where the job
              stops, because the next `call` resets the machine and these
              frames are not built for the synthesiser

    python3 tools/fw_oracle/gen_tone_jobs.py /tmp/tone.job
    EMU_PROJ=dm32uv-emu-1 $REVENG/tools/emu/run.sh /tmp/tone.job /tmp/tone.out
    python3 tools/fw_oracle/gen_tone_jobs.py --export /tmp/tone.out \\
            tests/fixtures/frame_tone.fw

SPDX-License-Identifier: ISC
"""
import os, struct, sys

REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))
import emu

SYNTH    = 0x00019DB8
BRK_PRE  = 0x00019E26          # the block the tone branch is handed
BRK_POST = 0x00019EAA          # and the block it leaves

CTX  = 0x00052000              # 0x800, above the frame jobs' scratch
BLK  = 0x00053000
PCM  = 0x00053200
NVST = 0x22C
FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "..", "tests", "fixtures", "dm32_arc4_1.fwframe")


def _base_state():
    """The channel state from the sequence fixture's `S` line, as a context."""
    for line in open(FIXTURE):
        if line.startswith("S "):
            v = [int(x) for x in line.split()[1:]]
            break
    else:
        raise SystemExit("no S line in %s" % FIXTURE)
    ctx = bytearray(0x800)
    o = 0
    struct.pack_into("<6i", ctx, 0x000, *v[o:o + 6]);            o += 6
    struct.pack_into("<%dh" % NVST, ctx, 0x018, *v[o:o + NVST]); o += NVST
    struct.pack_into("<68h", ctx, 0x470, *v[o:o + 68]);          o += 68
    struct.pack_into("<68h", ctx, 0x4f8, *v[o:o + 68]);          o += 68
    struct.pack_into("<170h", ctx, 0x648, *v[o:o + 170]);        o += 170
    struct.pack_into("<3h", ctx, 0x7be, *v[o:o + 3])
    return ctx


def _base_block():
    """The first voice block of the capture, as something to make a tone of."""
    for line in open(FIXTURE):
        if line.startswith("C "):
            f = line.split()
            for k in range(5, len(f)):
                if int(f[k]) == 1 and int(f[k + 1]) == 255:
                    return [int(x) for x in f[5:5 + 68]]
            return [int(x) for x in f[5:5 + 68]]
    raise SystemExit("no C line in %s" % FIXTURE)


def _tone_class(code):
    if (code - 5) & 0xFFFF < 0x76:
        return 0
    if (code - 0x80) & 0xFFFF < 0x10:
        return 1
    if (code - 0x90) & 0xFFFF < 0x10:
        return 2
    if (code - 0xA0) & 0xFFFF < 4:
        return 3
    return 4


def _ctcss_bin(code):
    t = (((code - 0x10000 if code & 0x8000 else code) * 0x5000) * 2) & 0xFFFFFFFF
    t = t - 0x100000000 if t & 0x80000000 else t
    v = (((t >> 3) + 0xFFFF) >> 16) - 1
    v = v - 0x10000 if v & 0x8000 else v
    return 0 if v < 0 else v


def _sdiv(a, b):
    """Math_SDiv 0x00018D74, enough of it to filter cases with."""
    if b == 0:
        return None                       # `divs` traps
    sign = (a < 0) != (b < 0)
    ua, ub = abs(a), abs(b)
    if ua < ub:
        return 0
    return -(ua // ub) if sign else ua // ub


def cases():
    """(toneMode, prev, block).

    The two halves of the branch want different things.

    Tone SHAPING, which is every mode but 1, wants codes from all five of the
    classifier's families, because the two corrections after the notch are
    keyed to code RANGES - 0x80..0x9F merges a DCS pair, 0x14..0x16 recomputes
    the pitch - and those ranges cut across the families rather than following
    them.  It also wants a range of envelopes, because what the notch does is
    only visible against something.

    Tone CONTINUATION, mode 1, runs only when this frame and the previous one
    are both CTCSS, so its cases have to build the previous frame too.  Its one
    trap is that prev[1] is BOTH the code the classifier reads AND the divisor
    of the index arithmetic - the stock code uses the same field for both - so
    a case cannot separate them and neither does this.  The index it computes
    is re-derived here to filter, because Vocoder_UpdatePitchHistoryBuffer does
    not range-check it and the stock code would write past a 0x38-entry array.
    """
    base = _base_block()
    out = []

    def blk(cls, code, p3, L, amps=None):
        b = list(base)
        b[0] = cls
        b[1] = code
        b[2] = L
        b[3] = p3
        if amps is not None:
            for i in range(0x38):
                b[8 + i] = amps(i)
        return b

    # --- tone shaping ------------------------------------------------------
    codes = [0, 1, 4, 5, 0x13, 0x14, 0x15, 0x16, 0x17, 0x40, 0x79, 0x7a, 0x7b,
             0x80, 0x81, 0x8f, 0x90, 0x9e, 0x9f, 0xa0, 0xa3, 0xa4, 0xc0, 0xfe]
    shapes = [None,
              lambda i: 0x7FFF,
              lambda i: max(1, 0x4000 >> (i // 5))]
    prev0 = blk(1, 0xff, 0, 0x20)
    for code in codes:
        p3 = _ctcss_bin(code) if _tone_class(code) == 0 else 0x0508
        for L in (1, 9, 0x38):
            for amps in shapes:
                out.append((0, list(prev0), blk(3, code, p3, L, amps)))
    # the branch is chosen by [1], not by [0], so a voice- or silence-class
    # block carrying a tone code takes it too
    for cls in (1, 2):
        for code in (0x14, 0x40, 0x88, 0xa1):
            p3 = _ctcss_bin(code) if _tone_class(code) == 0 else 0x0508
            out.append((0, list(prev0), blk(cls, code, p3, 0x20)))
    # a mode that is neither 0 nor 1, which the stock code shapes like 0
    out.append((2, list(prev0), blk(3, 0x40, _ctcss_bin(0x40), 0x20)))
    out.append((-1, list(prev0), blk(3, 0x91, 0x0508, 0x20)))

    # --- tone continuation -------------------------------------------------
    def index_after(code, pidx, pcode):
        q = _sdiv((pidx + 1) * code + pcode // 2, pcode)
        if q is None:
            return None
        q &= 0xFFFF
        t = (q - 0x10000 if q & 0x8000 else q) * 16
        while t <= code:
            t += 16
            q = (q + 1) & 0xFFFF
        return (q - 1) & 0xFFFF

    for pcls in (3, 1):                        # only 3 enters the branch
        for pcode in (5, 0x40, 0x7a, 0xff):
            for pidx in (0, 1, 0x20, 0x37):
                for code in (5, 0x20, 0x40, 0x7a, 0xfe):
                    for p3 in (0, 0x20, 0x37):
                        idx = index_after(code, pidx, pcode)
                        if idx is None or idx >= 0x38:
                            continue
                        prev = blk(pcls, pcode, pidx, 0x20)
                        out.append((1, prev, blk(3, code, p3, 0x20)))

    # The grid above cannot separate the early path's two gates from the
    # general one, and mutation says so: `|params[3] - prev[3]| < 2` widened to
    # 3 and `(prev[3] + 1) * 16` halved to 8 both leave it at 616 of 616.  The
    # first needs a difference of exactly 2 and the second a code between
    # (prev[3] + 1) * 8 and (prev[3] + 1) * 16, and no combination of
    # {0, 0x20, 0x37} against {0, 1, 0x20, 0x37} produces either.  These do.
    for pidx in (0, 1, 2, 5, 0x20, 0x37):
        for d in (0, 1, 2, 3):
            for p3 in (pidx + d, pidx - d):
                if not 0 <= p3 < 0x38:
                    continue
                for code in ((pidx + 1) * 8 - 1, (pidx + 1) * 8,
                             (pidx + 1) * 12, (pidx + 1) * 16 - 1,
                             (pidx + 1) * 16, (pidx + 1) * 16 + 1):
                    if not 5 <= code <= 0x7a:      # or it is not CTCSS at all
                        continue
                    for pcode in (5, 0x40, 0x7a):
                        idx = index_after(code, pidx, pcode)
                        if idx is None or idx >= 0x38:
                            continue
                        out.append((1, blk(3, pcode, pidx, 0x20),
                                    blk(3, code, p3, 0x20)))
    return out


def gen(jobfile):
    ctx = _base_state()
    j = emu.Job(maxsteps=2_000_000_000)
    j.hook_break(BRK_PRE)
    j.hook_break(BRK_POST)
    cs = cases()
    for i, (mode, prev, blk) in enumerate(cs):
        c = bytearray(ctx)
        struct.pack_into("<h", c, 0x7a0, mode)
        struct.pack_into("<h", c, 0x7ba, 0)
        struct.pack_into("<68h", c, 0x470,
                         *[v - 0x10000 if v > 0x7FFF else v for v in prev])
        j.poke(CTX, bytes(c))
        j.poke(BLK, struct.pack("<68h",
                                *[v - 0x10000 if v > 0x7FFF else v for v in blk]))
        j.poke(PCM, b"\xEE" * 0xA0)
        j.call(SYNTH, BLK, PCM, 0x50, 0, CTX)
        j.peek("pre%d" % i, BLK, 68 * 2)
        j.resume()
        j.peek("post%d" % i, BLK, 68 * 2)
        # and no third resume: the next `call` resets sp, lr and pc anyway, so
        # stopping here never enters the synthesis below.  That matters - these
        # frames are constructed for the tone branch and not for the
        # synthesiser, and running them into it divides by zero at 0x00018F14
        # on 467 of the 616, which is a property of the inputs and not of the
        # branch under test.
    j.write(jobfile)
    print("wrote %s: %d constructed tone frames" % (jobfile, len(cs)))


def export(outfile, dest):
    res = emu.parse(outfile)
    assert not res["faults"], res["faults"][:2]
    assert not res["errors"], res["errors"][:2]
    cs = cases()
    with open(dest, "w") as fh:
        fh.write("# The tone branch of Vocoder_SynthesizeFrame 0x00019DB8, on\n"
                 "# frames constructed for it: the corpus contains none.\n"
                 "# tools/fw_oracle/gen_tone_jobs.py.\n"
                 "# per record: toneMode (ctx+0x7a0), 68 shorts of the previous\n"
                 "#   block (ctx+0x470), 68 shorts of the block as the branch is\n"
                 "#   handed it at 0x00019E26, then the 68 it leaves at 0x00019EAA.\n")
        n = 0
        for i, (mode, prev, _) in enumerate(cs):
            pre  = struct.unpack("<68h", res["peek"]["pre%d" % i][0])
            post = struct.unpack("<68h", res["peek"]["post%d" % i][0])
            fh.write("%d %s %s %s\n" % (
                mode, " ".join(map(str, prev)), " ".join(map(str, pre)),
                " ".join(map(str, post))))
            n += 1
    print("%s: %d cases" % (dest, n))


if __name__ == "__main__":
    if sys.argv[1] == "--export":
        export(sys.argv[2], sys.argv[3])
    else:
        gen(sys.argv[1])
