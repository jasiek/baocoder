#!/usr/bin/env python3
"""Convert an SDRTrunk .mbe JSON export into the ambe_decode frames format.

Each output line is the 18-hex-character on-air AMBE frame followed by the
DMRA message indicator when the capture carries one, or '-' when it does not.

Usage: mbe_to_frames.py <in.mbe> [out.txt]
"""
import json
import sys


def main():
    src = json.load(open(sys.argv[1]))
    out = open(sys.argv[2], "w") if len(sys.argv) > 2 else sys.stdout
    out.write("# %s %s -> %s\n" % (src.get("protocol", "?"),
                                   src.get("from", "?"), src.get("to", "?")))
    for f in src["frames"]:
        out.write("%s %s\n" % (f["hex"], f.get("encryption_mi", "-")))


if __name__ == "__main__":
    main()
