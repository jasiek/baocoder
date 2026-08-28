#!/usr/bin/env python3
"""Regenerate tests/fixtures from the upstream known-key sample capture.

Fixtures produced (all small, all committed):

  <name>.frames   on-air 9-byte DMR AMBE frames, hex, one per line, with the
                  superframe message indicator where the capture carries one
  <name>.key      the traffic key and algorithm for that capture
  <name>.fec49    mbelib's 49-bit payload for each frame, plus its two Golay
                  error counts  -- the known-good side of the FEC test
  <name>.ambe49   the same 49 bits after the DMRA keystream is removed
  <name>.parms    mbelib's decoded model parameters for each plaintext frame
  <name>.pcm      mbelib's synthesised PCM, signed 16-bit little endian, 8 kHz

Provenance: the capture is dmr_arc4_1 from
https://github.com/tylerwatt12/known-key-mbe-samples, whose README records the
DMR samples as coming from a Baofeng DM-32 -- the same radio family this
project reverse-engineers.  The repository carries no licence, so it is fetched
rather than vendored; only the derived vectors above are committed.

Usage: make_fixtures.py <samples-repo> <fixture-dir>
"""
import json
import os
import re
import subprocess
import sys

SAMPLE = "dmr_arc4_1"
NAME = "dm32_arc4_1"
SUPERFRAME = 18


def rc4_keystream(key, drop, n):
    S = list(range(256))
    j = 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 0xFF
        S[i], S[j] = S[j], S[i]
    out = bytearray()
    i = j = 0
    for c in range(n + drop):
        i = (i + 1) & 0xFF
        j = (j + S[i]) & 0xFF
        S[i], S[j] = S[j], S[i]
        if c >= drop:
            out.append(S[(S[i] + S[j]) & 0xFF])
    return bytes(out)


def to_bits(data):
    return [(b >> k) & 1 for b in data for k in range(7, -1, -1)]


def main():
    samples, outdir = sys.argv[1], sys.argv[2]
    src = os.path.join(samples, SAMPLE)
    meta = dict(
        line.strip().split("=", 1)
        for line in open(os.path.join(src, "key.txt"))
        if "=" in line
    )
    key = bytes.fromhex(meta["key_hex"])
    frames = json.load(open(os.path.join(src, "encrypted.mbe")))["frames"]

    first = next(i for i, f in enumerate(frames) if "encryption_mi" in f)
    n = ((len(frames) - first) // SUPERFRAME) * SUPERFRAME
    sel = frames[first:first + n]

    os.makedirs(outdir, exist_ok=True)
    base = os.path.join(outdir, NAME)

    with open(base + ".frames", "w") as fh:
        fh.write("# on-air DMR AMBE+2 frames, 9 bytes each, from %s/%s\n"
                 "# columns: frame-hex  message-indicator-or-dash\n" % (samples, SAMPLE))
        for f in sel:
            fh.write("%s %s\n" % (f["hex"], f.get("encryption_mi", "-")))

    with open(base + ".key", "w") as fh:
        for k in ("protocol", "algorithm", "algid", "key_id", "key_hex"):
            fh.write("%s=%s\n" % (k, meta[k]))

    # mbelib: on-air frame -> 49 payload bits (still encrypted)
    tmp = base + ".hexonly"
    with open(tmp, "w") as fh:
        for f in sel:
            fh.write(f["hex"] + "\n")
    fec = subprocess.check_output(["tools/mbe_ref", "fec", tmp]).decode()
    os.unlink(tmp)
    with open(base + ".fec49", "w") as fh:
        fh.write(fec)

    # remove the DMRA keystream
    plain = []
    ksbits, pos = None, 0
    for i, (f, line) in enumerate(zip(sel, fec.strip().split("\n"))):
        if "encryption_mi" in f:
            mi = bytes.fromhex(f["encryption_mi"])
            ksbits = to_bits(rc4_keystream(key + mi, 256, 200))
            pos = 0
        bits = [int(c) for c in line.split()[0]]
        plain.append("".join(str(b ^ ksbits[pos + k]) for k, b in enumerate(bits)))
        pos += 56
    with open(base + ".ambe49", "w") as fh:
        fh.write("\n".join(plain) + "\n")

    # mbelib: 49 plaintext bits -> model parameters + PCM
    parms = subprocess.check_output(
        ["tools/mbe_ref", "decode", base + ".ambe49", base + ".pcm"]).decode()
    with open(base + ".parms", "w") as fh:
        fh.write(parms)

    write_table_reference(outdir)
    print("wrote %d frames to %s.*" % (len(sel), base))


def write_table_reference(outdir):
    """mbelib's float values for the tables tools/extract_tables.py pulls out of
    the firmware, so tests/test_tables.c can check the extraction without the
    library being present."""
    import glob
    root = None
    for cand in ("third_party/mbelib", "../mbelib"):
        if os.path.exists(os.path.join(cand, "ambe3600x2450_const.h")):
            root = cand
            break
    if root is None:
        print("  (mbelib not found; skipping table reference)")
        return
    src = open(os.path.join(root, "ambe3600x2450_const.h")).read()

    def grab(decl):
        i = src.index(decl)
        j = src.index("{", i)
        k = src.index("};", j)
        return [float(x) for x in re.findall(r"-?\d+\.?\d*", src[j:k])]

    want = [("prba24", "AmbePRBA24[512][3]"), ("prba58", "AmbePRBA58[128][4]"),
            ("hoc_b5", "AmbeHOCb5[32][4]"), ("hoc_b6", "AmbeHOCb6[16][4]"),
            ("hoc_b7", "AmbeHOCb7[16][4]"), ("hoc_b8", "AmbeHOCb8[8][4]"),
            ("dg", "AmbeDg[32]"), ("lmprbl", "AmbeLmprbl[57][4]"),
            ("vuv", "const int AmbeVuv[32][8]")]
    with open(os.path.join(outdir, "mbelib_tables.txt"), "w") as fh:
        fh.write("# mbelib reference values for the tables extracted from the "
                 "firmware image.\n# one table per line: name count v0 v1 ...\n")
        for name, decl in want:
            v = grab(decl)
            fh.write("%s %d %s\n" % (name, len(v), " ".join("%.9g" % x for x in v)))


if __name__ == "__main__":
    main()
