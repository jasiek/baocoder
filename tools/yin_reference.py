"""YIN on the decoded corpus - a second opinion on the analyser's pitch.

mbelib cannot do this job: it is a decoder, so it dequantises pitch but never
estimates it from audio.  The decode side is already checked against it
(test_params, w0 agreeing to 3.1e-06); the open question was about the
*estimator*, and there is no reference implementation to compare it with.

So this is an independent algorithm rather than an independent implementation:
YIN (de Cheveigne & Kawahara 2002), whose cumulative-mean-normalised difference
is the step that suppresses the octave errors a plain correlation makes.  The
point is not to beat the analyser - it does not, scoring 80.7% within four
quantiser steps against 86.3% - but to ask whether the frames the analyser gets
wrong are hard or merely mishandled.

They are hard.  On the 153 frames where the analyser is off by more than four
steps, YIN is also off by more than four on 92.8% of them, and an oracle
picking the better of the two reaches 87.3% against 86.3%.  YIN's own
aperiodicity score says why: median 0.670 on those frames against 0.084 on the
rest, with 86.9% of them above the 0.15 at which YIN itself declares a frame
unvoiced.  Aperiodicity predicts the analyser's failure with AUC 0.861.

The audio on those frames does not carry a recoverable pitch, so 86.3% is near
the ceiling for this corpus rather than a shortfall to be engineered away.

    python3 tools/yin_reference.py <dir>      # from tools/dump_corpus.c
"""
import sys, glob, os
import numpy as np

FS=8000; FRAME=160
TAU_MIN, TAU_MAX = 20, 124      # 400 Hz down to 64 Hz, the codec's own span
W = 200                          # analysis window

tab=[l.split() for l in open('/tmp/b0.txt')]
B0HZ=np.array([float(x[1]) for x in tab])

def b0_of(hz):
    return int(np.argmin(np.abs(B0HZ-hz)))

def yin(x, thr=0.15):
    x = x.astype(np.float64)
    if len(x) < W+TAU_MAX: return None
    d = np.empty(TAU_MAX+1)
    d[0]=0.0
    for t in range(1, TAU_MAX+1):
        diff = x[:W] - x[t:t+W]
        d[t] = np.dot(diff,diff)
    # cumulative mean normalised difference
    cm = np.empty_like(d); cm[0]=1.0
    run = np.cumsum(d[1:])
    cm[1:] = d[1:] * np.arange(1,TAU_MAX+1) / np.maximum(run,1e-30)
    cand = None
    for t in range(TAU_MIN, TAU_MAX):
        if cm[t] < thr:
            while t+1 < TAU_MAX and cm[t+1] < cm[t]: t += 1
            cand = t; break
    if cand is None:
        cand = TAU_MIN + int(np.argmin(cm[TAU_MIN:TAU_MAX]))
    t = cand
    if 1 <= t < TAU_MAX-1:                      # parabolic refine
        y0,y1,y2 = cm[t-1],cm[t],cm[t+1]
        den = 2*(y0-2*y1+y2)
        if den != 0:
            s = (y0-y2)/den
            if -1 < s < 1: t = t + s
    return FS/float(t), cm[cand]

rows=[]
for meta in sorted(glob.glob(os.path.join(sys.argv[1],"*.meta"))):
    pcm = np.fromfile(meta[:-5]+".pcm", dtype="<i2")
    for ln in open(meta):
        p=ln.split()
        idx,f0q,L,b1 = int(p[0]),int(p[1]),int(p[2]),int(p[3])
        if b1 < 0: continue
        end=(idx+1)*FRAME
        lo = end-(W+TAU_MAX)
        seg = pcm[lo:end] if lo >= 0 else np.concatenate(
                  [np.zeros(-lo, dtype=pcm.dtype), pcm[:end]])
        r = yin(seg)
        hz, conf = r if r is not None else (0.0, 1.0)
        truehz = f0q/float(1<<19)*FS
        rows.append((b0_of(truehz), b0_of(hz), conf, truehz, hz))

print("YIN on the decoded corpus, %d frames"%len(rows))
t=np.array([r[0] for r in rows]); y=np.array([r[1] for r in rows])
conf=np.array([r[2] for r in rows])
dy=np.abs(y-t)
for k in (1,2,4,8):
    print("   within %d steps of the transmitted b0: %5.1f%%"%(k,100*(dy<=k).mean()))
np.save('/tmp/yin.npy', np.array([t,y,conf]))
