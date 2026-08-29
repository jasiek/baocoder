#!/usr/bin/env python3
"""Measure what fixed point cost, and what it bought.

    tools/compare_precision.py [--float-worktree ../baocoder] [--work DIR]

Builds four decoders and runs all six captures through each:

  fixed     this branch
  float     the float library on the float worktree
  oracle    that same float source retyped to double (tools/make_oracle.py)
  oracle-fw the same, but evaluating 2^x for the pitch with the radio's own
            polynomial instead of pow()

The last one is not redundant.  The fundamental feeds a phase accumulator that
runs the length of the transmission, so a 4e-4 relative difference in f0 is a
growing time shift in the audio - tens of dB of apparent error that says
nothing about how the arithmetic is carried.  Comparing against `oracle`
measures the pitch law; comparing against `oracle-fw` measures the arithmetic.
Reporting only the first blames fixed point for a difference in the pitch law;
reporting only the second hides that difference.  Both are printed.

mbelib appears here too, from the committed .parms and .pcm fixtures, but as a
third independent implementation rather than as ground truth: it is float, and
it reconstructs the codebooks itself where this decoder reads the radio's Q11
tables, so a half-LSB baseline disagreement sits under every number in that
column.

The audio is compared sample by sample, which is only legitimate because the
noise generator is integer and identical on both sides: the same draws come
out in the same order as long as L and the voicing decisions agree.  The
harness checks that they do and says so when they do not.

SPDX-License-Identifier: ISC
"""
import math
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BRANCH = os.path.dirname(HERE)
CFLAGS = ["-O2", "-std=c99", "-D_DEFAULT_SOURCE"]


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit("failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    return r


def build(work, float_tree):
    """Build the four dump binaries.  Returns {name: path}."""
    out = {}

    run(["make", "-s", "libbaocoder.a"], cwd=BRANCH)
    out["fixed"] = os.path.join(work, "dump_fixed")
    run(["cc"] + CFLAGS + ["-I%s/include" % BRANCH, "-I%s/src" % BRANCH,
         "-o", out["fixed"], "%s/tools/ambe_dump.c" % BRANCH,
         "%s/libbaocoder.a" % BRANCH])

    run(["make", "-s", "libbaocoder.a"], cwd=float_tree)
    out["float"] = os.path.join(work, "dump_float")
    run(["cc"] + CFLAGS + ["-DAMBE_FLOAT_API", "-I%s/include" % float_tree,
         "-o", out["float"], "%s/tools/ambe_dump.c" % BRANCH,
         "%s/libbaocoder.a" % float_tree, "-lm"])

    for name, flags in (("oracle", []), ("oracle-fw", ["--pitch=firmware"])):
        d = os.path.join(work, name.replace("-", "_"))
        shutil.rmtree(d, ignore_errors=True)
        run(["python3", "%s/tools/make_oracle.py" % BRANCH, float_tree, d] + flags)
        out[name] = os.path.join(work, "dump_" + name.replace("-", "_"))
        run(["cc"] + CFLAGS + ["-DAMBE_FLOAT_API", "-I" + d, "-o", out[name],
             "%s/tools/ambe_dump.c" % BRANCH] +
            [os.path.join(d, f) for f in ("ambe_params.c", "ambe_synth.c",
                                          "ambe_decoder.c", "ambe_fec.c",
                                          "golay.c", "ambe_tables_fw.c")] + ["-lm"])
    return out


def captures(fixtures):
    out = []
    with open(os.path.join(fixtures, "captures.txt")) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            out.append(line.split()[0])
    return out


def load_parms(path):
    frames = []
    with open(path) as f:
        for line in f:
            t = line.split()
            L = int(t[2])
            frames.append(dict(
                n=int(t[0]), type=int(t[1]), L=L,
                w0=float(t[3]), gamma=float(t[4]),
                V=[int(v) for v in t[5:5 + L]],
                log2Ml=[float(v) for v in t[5 + L:5 + 2 * L]],
                Ml=[float(v) for v in t[5 + 2 * L:5 + 3 * L]]))
    return frames


def load_pcm(path):
    with open(path, "rb") as f:
        raw = f.read()
    return struct.unpack("<%dh" % (len(raw) // 2), raw[:len(raw) // 2 * 2])


def dev(a, b):
    """Relative deviation, floored at 1 so near-zero values do not blow up.
    The same measure tests/test_params.c uses, so the numbers are comparable."""
    return abs(a - b) / max(abs(a), abs(b), 1.0)


def compare_parms(ref, got):
    st = dict(L=0, V=0, w0=0.0, gamma=0.0, log2Ml=0.0, Ml=0.0,
              log2Ml_rms=0.0, n=0)
    sq = 0.0
    for r, g in zip(ref, got):
        if r["L"] != g["L"]:
            st["L"] += 1
            continue
        if r["V"] != g["V"]:
            st["V"] += 1
        st["w0"] = max(st["w0"], dev(r["w0"], g["w0"]))
        st["gamma"] = max(st["gamma"], dev(r["gamma"], g["gamma"]))
        for u, v in zip(r["log2Ml"], g["log2Ml"]):
            d = dev(u, v)
            st["log2Ml"] = max(st["log2Ml"], d)
            sq += d * d
            st["n"] += 1
        for u, v in zip(r["Ml"], g["Ml"]):
            st["Ml"] = max(st["Ml"], dev(u, v))
    st["log2Ml_rms"] = math.sqrt(sq / st["n"]) if st["n"] else 0.0
    return st


def snr(ref, got):
    n = min(len(ref), len(got))
    num = sum(float(v) * v for v in ref[:n])
    den = sum((float(a) - b) ** 2 for a, b in zip(ref[:n], got[:n]))
    if den == 0:
        return float("inf")
    if num == 0:
        return float("-inf")
    return 10.0 * math.log10(num / den)


def main():
    argv = sys.argv[1:]
    float_tree = os.path.abspath(
        argv[argv.index("--float-worktree") + 1] if "--float-worktree" in argv
        else os.path.join(BRANCH, "..", "baocoder"))
    work = (argv[argv.index("--work") + 1] if "--work" in argv
            else os.path.join(BRANCH, ".compare"))
    os.makedirs(work, exist_ok=True)
    fixtures = os.path.join(BRANCH, "tests", "fixtures")

    print("building four decoders ...", file=sys.stderr)
    bins = build(work, float_tree)
    names = ["fixed", "float", "oracle", "oracle-fw"]

    caps = captures(fixtures)
    data = {k: {} for k in names}
    pcm = {k: {} for k in names}
    for cap in caps:
        src = os.path.join(fixtures, cap + ".ambe49")
        for k in names:
            p = os.path.join(work, "%s.%s" % (cap, k))
            run([bins[k], src, p + ".parms", p + ".pcm"])
            data[k][cap] = load_parms(p + ".parms")
            pcm[k][cap] = load_pcm(p + ".pcm")

    total = sum(len(data["fixed"][c]) for c in caps)
    print()
    print("%d frames over %d captures" % (total, len(caps)))

    # ---- parameters, against each reference
    print()
    print("Model parameters, worst deviation over every frame and harmonic")
    print()
    hdr = "%-22s %5s %5s %10s %10s %10s %10s" % (
        "", "dL", "dV", "w0", "gamma", "log2Ml", "Ml")
    for refname in ("oracle", "oracle-fw"):
        print("  vs the %s oracle%s" %
              ("double-precision" if refname == "oracle" else "double-precision",
               "" if refname == "oracle" else ", firmware pitch law"))
        print("  " + hdr)
        for k in ("float", "fixed"):
            agg = dict(L=0, V=0, w0=0.0, gamma=0.0, log2Ml=0.0, Ml=0.0)
            for cap in caps:
                st = compare_parms(data[refname][cap], data[k][cap])
                agg["L"] += st["L"]
                agg["V"] += st["V"]
                for f in ("w0", "gamma", "log2Ml", "Ml"):
                    agg[f] = max(agg[f], st[f])
            print("  %-22s %5d %5d %10.2e %10.2e %10.2e %10.2e" %
                  (k, agg["L"], agg["V"], agg["w0"], agg["gamma"],
                   agg["log2Ml"], agg["Ml"]))
        print()

    # ---- audio
    print("Synthesised audio, sample by sample")
    print()
    print("  %-30s %10s %10s" % ("", "SNR dB", "worst frame"))
    pairs = [("float", "oracle", "float vs oracle"),
             ("fixed", "oracle", "fixed vs oracle"),
             ("fixed", "oracle-fw", "fixed vs oracle (fw pitch)"),
             ("oracle-fw", "oracle", "the pitch law alone")]
    for a, b, label in pairs:
        allref, allgot, worst = [], [], 999.0
        for cap in caps:
            allref += pcm[b][cap]
            allgot += pcm[a][cap]
            for i in range(len(pcm[b][cap]) // 160):
                s = snr(pcm[b][cap][i * 160:(i + 1) * 160],
                        pcm[a][cap][i * 160:(i + 1) * 160])
                if s < worst:
                    worst = s
        print("  %-30s %10.1f %10.1f" % (label, snr(allref, allgot), worst))

    # ---- did the noise generators stay in step?
    print()
    desync = 0
    for cap in caps:
        for r, g in zip(data["oracle-fw"][cap], data["fixed"][cap]):
            if r["L"] != g["L"] or r["V"] != g["V"]:
                desync += 1
    print("  RNG lockstep: %d of %d frames differ in L or voicing" % (desync, total))
    print("  (the sample-by-sample comparison above is only meaningful at 0)")

    # ---- mbelib, where a fixture exists
    ref = os.path.join(fixtures, "dm32_arc4_1.parms")
    if os.path.exists(ref):
        print()
        print("Against mbelib - a third implementation, not ground truth: it is")
        print("float, and reconstructs the codebooks where this reads the radio's")
        print("Q11 tables, so a half-LSB disagreement underlies every figure.")
        print()
        mb = []
        with open(ref) as f:
            for line in f:
                t = line.split()
                bad = int(t[0])
                if bad:
                    mb.append(None)
                    continue
                L = int(t[2])
                mb.append(dict(w0=float(t[1]), L=L, gamma=float(t[3]),
                               V=[int(float(v)) for v in t[4:4 + L]],
                               Ml=[float(v) for v in t[4 + L:4 + 2 * L]],
                               log2Ml=[float(v) for v in t[4 + 2 * L:4 + 3 * L]]))
        print("  %-22s %5s %10s %10s %10s" % ("", "dL", "w0", "gamma", "log2Ml"))
        for k in ("float", "fixed"):
            dl = 0
            mw = mg = ml = 0.0
            for r, g in zip(mb, data[k]["dm32_arc4_1"]):
                if r is None:
                    continue
                if r["L"] != g["L"]:
                    dl += 1
                    continue
                mw = max(mw, dev(r["w0"], g["w0"]))
                mg = max(mg, dev(r["gamma"], g["gamma"]))
                for u, v in zip(r["log2Ml"], g["log2Ml"]):
                    ml = max(ml, dev(u, v))
            print("  %-22s %5d %10.2e %10.2e %10.2e" % (k, dl, mw, mg, ml))


if __name__ == "__main__":
    main()
