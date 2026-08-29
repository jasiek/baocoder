#!/usr/bin/env python3
"""Build the double-precision reference decoder used by the precision comparison.

    tools/make_oracle.py <path-to-float-worktree> <out-dir>

The comparison needs something to be precise *relative to*.  Measuring the
fixed-point library against the float one says how far apart they are, not
which is closer to the exact result - and for a quantity like the synthesis
phase, which accumulates without bound from frame to frame, the float side is
not obviously the better of the two.

So the oracle is the float implementation's own source, retyped to double:
same algorithm, same constants, same order of operations, about fifteen
significant digits instead of seven.  Generating it rather than writing one by
hand is the whole point - a hand-written reference would be a second
implementation, and a disagreement would not say which side had the bug.

Two substitutions do it:

  * the type, `float` -> `double` on word boundaries, so that identifiers like
    ambe_float_to_s16 keep their names, and
  * the float literal suffix, `0.65f` -> `0.65`.  Missing this would leave
    every constant rounded to float and quietly halve the oracle's precision.

The script checks afterwards that neither survived, because a silent failure
here does not produce an error - it produces a reference that is no more
accurate than the thing it is meant to judge.  An earlier version of this used
sed, whose BSD build has no \\b, and did exactly that.
"""
import os
import re
import shutil
import sys

RETYPE = ["include/ambe.h", "src/ambe_params.c", "src/ambe_synth.c",
          "src/ambe_decoder.c"]

# Vocoder_PitchFromLog2 0x0002AD6C, for --pitch=firmware.  Kept here rather
# than shared with the library so the oracle stays a standalone artefact that
# depends on nothing this comparison is meant to judge.
FW_PITCH = """/* the radio's own 2^x, Vocoder_PitchFromLog2 0x0002AD6C */
static int fw_pitch_q19(short x)
{
    int iv = (short)x, t;
    short k;
    unsigned f;

    if (iv < -0x1000) {
        k = -4;
        do {
            x = (short)(unsigned short)(x + 0x1000);
            iv = (short)x;
            k = (short)(unsigned short)(k + 1);
        } while (iv < -0x1000);
    } else {
        k = -4;
    }
    t = ((iv * 0x13b) >> 12) + 0x71b;
    t = (short)(((t * iv) >> 12) + 0x1ec0);
    t = (short)(((t * iv) >> 12) + 0x58b9);
    f = (unsigned)(t * iv);
    f = (f >> 12) & 0xffff;
    f ^= 0x8000;
    return (int)((short)f) >> (k & 0x1f);
}

"""

PITCH_OLD = """    f0 = pow(2.0, (double)x / 4096.0);
    f0q19 = (int)(f0 * 524288.0 + 0.5);"""
PITCH_NEW = """    f0q19 = fw_pitch_q19((short)x);"""
VERBATIM = ["src/ambe_fec.c", "src/golay.c", "src/ambe_tables_fw.c",
            "src/ambe_bitpos.h"]

FLOAT_WORD = re.compile(r"\bfloat\b")
FLOAT_LIT = re.compile(r"(?<![\w.])(\d+\.?\d*(?:[eE][-+]?\d+)?)[fF](?![\w.])")


def retype(text):
    text = FLOAT_WORD.sub("double", text)
    return FLOAT_LIT.sub(r"\1", text)


def use_firmware_pitch(body):
    if PITCH_OLD not in body:
        sys.exit("oracle: could not find the pitch expression to replace")
    body = body.replace(PITCH_OLD, PITCH_NEW, 1)
    body = body.replace("void ambe_pitch_from_b0(", FW_PITCH + "void ambe_pitch_from_b0(", 1)
    return body.replace("    *f0_out = (double)((double)f0q19 / 524288.0);",
                        "    f0 = (double)f0q19 / 524288.0;\n    *f0_out = f0;")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    fw_pitch = "--pitch=firmware" in flags
    src = args[0] if len(args) > 0 else "../baocoder"
    out = args[1] if len(args) > 1 else "oracle"

    if not os.path.exists(os.path.join(src, "src/ambe_params.c")):
        sys.exit("no float worktree at %s" % src)
    os.makedirs(out, exist_ok=True)

    for rel in RETYPE:
        with open(os.path.join(src, rel)) as f:
            body = retype(f.read())
        if fw_pitch and rel.endswith("ambe_params.c"):
            body = use_firmware_pitch(body)
        with open(os.path.join(out, os.path.basename(rel)), "w") as f:
            f.write(body)
    for rel in VERBATIM:
        shutil.copy(os.path.join(src, rel), out)

    # the tables header carries the Q11 scale as a float constant
    p = os.path.join(out, "ambe_tables.h")
    with open(os.path.join(src, "src/ambe_tables.h")) as f:
        body = retype(f.read())
    with open(p, "w") as f:
        f.write(body)

    bad = 0
    for name in os.listdir(out):
        if not name.endswith((".c", ".h")):
            continue
        with open(os.path.join(out, name)) as f:
            body = f.read()
        for pat, what in ((FLOAT_WORD, "float type"), (FLOAT_LIT, "float literal")):
            for m in pat.finditer(body):
                line = body.count("\n", 0, m.start()) + 1
                print("%s:%d: %s survived: %s" % (name, line, what, m.group(0)),
                      file=sys.stderr)
                bad += 1
    if bad:
        sys.exit("oracle would not be double precision: %d sites" % bad)
    print("oracle sources in %s (%s pitch)"
          % (out, "firmware" if fw_pitch else "exact"))


if __name__ == "__main__":
    main()
