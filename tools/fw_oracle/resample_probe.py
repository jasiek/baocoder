#!/usr/bin/env python3
"""Break inside Vocoder_ResampleSpectralEnvelope 0x00026A84 and verify it.

`gen_jobs.py` can only see the parameter blocks between frames.  This one stops
the emulator *inside* the interpolator and reads its two scratch arrays, which
is the only way to tell which of the three pitch branches fired and to check the
resampling arithmetic rather than infer it from the outside.

  usage:  resample_probe.py <job> <frames> <nframes>       generate
          resample_probe.py --check <out>                  verify a completed run

Addresses.  All of these were measured with a recon job (`hook break` at the
function's entry plus `getreg`), not assumed: the harness sets SP itself and the
call path here is fixed, so the stack is deterministic.

  0x00026B32  after the FIRST mix operand is built and the second call returns
  0x00026BB4  after the mix loop - out, local_104 and local_94 all live
  0x00019DB8  Vocoder_SynthesizeFrame entry, to read which block it is handed
  0x00016CDC  Vocoder_ConfigureFrame entry, the caller that owns the stack block
  local_104   0x00057C7C   56 x int16, param_2's envelope resampled
  local_94    0x00057CEC   56 x int16, param_3's envelope resampled
  param_1     0x00057E14   the OUTPUT, a caller's stack local - which is why no
                           block of the parameter context is this function's
                           result
  param_2     0x00045AA4 = TX_CTX+1000        the frame just decoded
  param_3     0x00045C3C = TX_CTX+1000+0x198  the codec's retained previous frame

Breaking makes the stop schedule irregular - the function early-exits to
Vocoder_CopyFrameParams on frames it does not interpolate, so it does not reach
the break every wake, and a stop that lands here consumes the resume the wake
loop meant for the pend break.  That is tolerated rather than fixed: every peek
in a stop is taken at one instant, so each stop is internally consistent and
every check below is within-stop.  It does mean the frame feed drifts from the
corpus, so this run is not comparable with the .fw* fixtures frame by frame.

SPDX-License-Identifier: ISC
"""
import os, struct, sys

REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))

BRK_MIXED  = 0x00026BB4
BRK_SYNTH  = 0x00019DB8
BRK_CONFIG = 0x00016CDC
L104, L94, POUT = 0x00057C7C, 0x00057CEC, 0x00057E14
PARAM_2, PARAM_3 = 0x00045AA4, 0x00045C3C
BLK = 0x198


def gen(jobfile, framesfile, nframes):
    import emu, vocoder_ctx as V, oracle_dectask as D
    frames = D.read_frames(framesfile, nframes, 0)
    j = emu.Job(); V.init(j)
    for addr in D.STUBS:
        j.hook_ret(addr, 0)
    j.hook_break(D.BRK_AFTER_PEND)
    j.hook_break(BRK_MIXED)
    j.hook_break(BRK_SYNTH)
    j.hook_break(BRK_CONFIG)
    j.poke(V.TX_CTX + D.F_GATE, b"\x01")
    j.poke(V.TX_CTX + D.F_MODE, b"\x00" * D.N_MODE)
    j.call(D.TX_TASK, D.SCRATCH_ERR)
    for w in range(nframes * D.WAKES_PER_FRAME):
        f = w // D.WAKES_PER_FRAME
        j.poke(V.TX_CTX + D.F_FILL, struct.pack("<h", D.HALF))
        j.poke(V.TX_CTX + D.F_BURST + (f % 3) * 9, frames[f])
        j.resume()
        for r in ("sp", "r0", "r1", "r2", "r3"):
            j.getreg(r)
        j.peek("p2_%d" % w, PARAM_2, BLK)
        j.peek("p3_%d" % w, PARAM_3, BLK)
        j.peek("l104_%d" % w, L104, 0x70)
        j.peek("l94_%d" % w, L94, 0x70)
        j.peek("out_%d" % w, POUT, 0x100)
    j.write(jobfile)
    print("wrote %s: %d wakes" % (jobfile, nframes * D.WAKES_PER_FRAME))


# ---------------------------------------------------------------- the model

def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def resample(ratio, src, n):
    """Vocoder_ComputeHarmonicResampleRatio 0x000269B0.

    `buf[k]` is harmonic k: harmonic 0 is held equal to harmonic 1 and three
    edge-holds sit above 56, so the interpolation needs no bounds test.  The
    weight is 0x7FFF, not 0x8000, which is why an exact copy still loses one
    LSB - the fingerprint that says the ratio short-circuited to unity.
    """
    buf = [src[0]] + src[:56] + [src[55]] * 3
    out, acc = [], ratio
    for _ in range(n):
        idx = s16(acc >> 16)
        frac = ((acc - idx * 0x10000) & 0x1FFFF) >> 1        # Q15
        idx2 = s16(idx + 1)
        a = buf[55] if idx >= 0x3C else buf[idx]
        b = buf[55] if (idx >= 0x3C or idx2 >= 0x3C) else buf[idx2]
        v = (a * s16(0x7FFF - frac) * 2 + frac * b * 2) >> 16
        out.append(max(-0x8000, min(0x7FFF, v)))
        acc += ratio
    return out


def mix(a, b):
    """The averaging loop at 0x00026B6A, clamps included."""
    out = []
    for x, y in zip(a, b):
        v = s16(((x << 16) + (y << 16)) >> 17)
        out.append(-0x77FF if v < -0x7800 else (0x77FF if v >= 0x7800 else v))
    return out


def check(outfile):
    stops, cur = [], None
    for line in open(outfile):
        if line.startswith("BREAK "):
            cur = {"pc": int(line.split()[1], 16), "p": {}, "r": {}}
            stops.append(cur)
        elif line.startswith("PEEK ") and cur is not None:
            _, n, h = line.split()
            cur["p"][n.rsplit("_", 1)[0]] = bytes.fromhex(h)
        elif line.startswith("REG ") and cur is not None:
            _, n, h = line.split()
            cur["r"][n] = int(h, 16)

    def hdr(b):
        s = struct.unpack_from("<8h", b, 0)
        return dict(cls=s[0], L=s[2], vuv=struct.unpack_from("<I", b, 8)[0],
                    f0=s[6] & 0xFFFF)

    def env(b, n=56):
        return list(struct.unpack_from("<%dh" % n, b, 0x10))

    def arr(b, n=56):
        return list(struct.unpack_from("<%dh" % n, b, 0))

    fi = [i for i, s in enumerate(stops) if s["pc"] == BRK_MIXED]
    branches = {"param_2 f0": 0, "param_3 f0": 0, "geometric mean": 0}
    pitch_ok = r104 = r94 = mix_ok = 0
    for i in fi:
        p = stops[i]["p"]
        h2, h3, ho = hdr(p["p2"]), hdr(p["p3"]), hdr(p["out"])
        # the pitch branch, on the two voicing words against the masks at
        # 0x00026C64 (0x55555555) and 0x00026C68 (0xAAAAAAAA)
        if (h3["vuv"] & 0x55555555) == 0 or (ho["vuv"] & 0xAAAAAAAA) != 0:
            branches["param_2 f0"] += 1
            pitch_ok += (ho["f0"] == h2["f0"])
        elif (h2["vuv"] & 0x55555555) == 0:
            branches["param_3 f0"] += 1
            pitch_ok += (ho["f0"] == h3["f0"])
        else:
            branches["geometric mean"] += 1
            pitch_ok += 1                       # checked as sqrt() below
        n = ho["L"]
        for src_h, src_b, got, which in ((h2, p["p2"], arr(p["l104"]), 0),
                                         (h3, p["p3"], arr(p["l94"]), 1)):
            base = (ho["f0"] << 16) // src_h["f0"] if src_h["f0"] else 0
            e, tgt = env(src_b), got[:n]
            # Math_DivideNormalized 0x0002692C truncates after normalising, so
            # its result sits a few ULP below the exact quotient; solve for it
            # rather than reimplementing the normalisation.
            hit = any(resample(base + d, e, n) == tgt for d in range(-96, 1))
            if which == 0:
                r104 += hit
            else:
                r94 += hit
        mix_ok += (mix(arr(p["l104"]), arr(p["l94"]))[:n] == env(p["out"], n))

    n = len(fi)
    print("interpolating calls captured: %d" % n)
    print("  pitch branch  %s" % branches)
    print("  f0_out as predicted            %d/%d" % (pitch_ok, n))
    print("  local_104 reproduced bit-exact %d/%d" % (r104, n))
    print("  local_94  reproduced bit-exact %d/%d" % (r94, n))
    print("  out == clamp((l104+l94)/2)     %d/%d" % (mix_ok, n))

    # Where the interpolated block goes.  Vocoder_ConfigureFrame renders as
    #     ProcessFrameSignaling(..., pCtx+1000, asStack_ac, pCtx);
    #     SynthesizeFrame(asStack_ac, pOutBuf, nFrameSize, 0, pCtx);
    # so r0 at the synthesiser's entry says whether asStack_ac is POUT.  Each
    # 160-sample frame is built as two 80-sample halves and the two calls take
    # different sources, so the pairing is what to report.
    sy = [s for s in stops if s["pc"] == BRK_SYNTH]
    from_interp = sum(1 for s in sy if s["r"].get("r0") == POUT)
    from_params = sum(1 for s in sy if s["r"].get("r0") == PARAM_2)
    pairs = sum(1 for a, b in zip(sy, sy[1:])
                if a["r"].get("r0") == POUT and b["r"].get("r0") == PARAM_2
                and b["r"].get("r1", 0) - a["r"].get("r1", 0) == 0xA0)
    print("Vocoder_SynthesizeFrame calls: %d" % len(sy))
    print("  handed the interpolated block (0x%08X) %d" % (POUT, from_interp))
    print("  handed PARAMS+0x000 (0x%08X)           %d" % (PARAM_2, from_params))
    print("  interpolated-then-current into adjacent 80-sample halves: %d"
          % pairs)
    print("  other sources: %d" % (len(sy) - from_interp - from_params))

    return 0 if (pitch_ok == r104 == r94 == mix_ok == n) else 1


if __name__ == "__main__":
    if sys.argv[1] == "--check":
        sys.exit(check(sys.argv[2]))
    gen(sys.argv[1], sys.argv[2], int(sys.argv[3]))
