#!/usr/bin/env python3
"""Generate an EmuRun job that decodes a .frames file through Vocoder_TxTask,
capturing a wide parameter window so voicing/gain/amplitudes can be located
offline from a completed run."""
import os, struct, sys
REVENG = os.environ.get("REVENG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                 "baofeng-dm32uv-reveng"))
sys.path.insert(0, os.path.join(REVENG, "tools", "emu"))
import emu, vocoder_ctx as V
import oracle_dectask as D

PAR_LEN = 0x400

def gen(jobfile, framesfile, nframes):
    frames = D.read_frames(framesfile, nframes, 0)
    j = emu.Job(); V.init(j)
    for addr in D.STUBS: j.hook_ret(addr, 0)
    j.hook_break(D.BRK_AFTER_PEND)
    j.poke(V.TX_CTX + D.F_GATE, b"\x01")
    j.poke(V.TX_CTX + D.F_MODE, b"\x00" * D.N_MODE)
    j.call(D.TX_TASK, D.SCRATCH_ERR)
    for w in range(nframes * D.WAKES_PER_FRAME):
        f = w // D.WAKES_PER_FRAME
        j.poke(V.TX_CTX + D.F_FILL, struct.pack("<h", D.HALF))
        j.poke(V.TX_CTX + D.F_BURST + (f % 3) * 9, frames[f])
        j.resume()
        j.peek("ix%d" % w, V.TX_CTX + D.F_PCM_IDX, 6)
        j.peek("pcm%d" % w, V.TX_CTX + D.PCM_RING, D.PCM_RING_LEN * 2)
        if w % 2 == 0:
            j.peek("bits%d" % f, V.TX_CTX + D.F_BITS, 49 * 2)
            j.peek("cur%d" % f, V.TX_CTX + D.F_BURST_CURSOR, 1)
        else:
            j.peek("par%d" % f, V.TX_CTX + D.PARAMS, PAR_LEN)
    j.write(jobfile)
    print("wrote %s: %d frames" % (jobfile, nframes))

if __name__ == "__main__":
    gen(sys.argv[1], sys.argv[2], int(sys.argv[3]))
