#!/usr/bin/env python3
"""Does resolution actually explain the voicing failure?

docs/fixed-point.md diagnoses the analyser's voicing defect as a resolution
problem: over 93% of the corpus's bands have fewer than 4 spectrum bins per
harmonic, so the +-step/4 window cannot isolate a harmonic from its neighbours
and the ratio saturates near 0.4 whatever the true voicing.  The proposed fix is
the radio's own filterbank, which measures on 58 decimated samples per channel -
464 original samples, 58 ms - against this library's 199-sample (25 ms) window.

Transcribing that filterbank exactly is days of work, so this asks the prior
question first, and isolates the one variable the diagnosis names.  It runs the
*same* measure - the same harmonic-centred +-step/4 numerator over the same
total - at a range of window lengths, and reports what the voiced/unvoiced
separation does.  If longer windows move the separation, resolution is the
cause and the filterbank will work.  If they do not, the filterbank will not fix
it either and the diagnosis needs revisiting.

Input is what tools/dump_corpus.c writes: continuous PCM plus per-frame truth
(f0, L, and the transmitted 8-band voicing pattern) taken from the radio's own
bitstream.

    ./dump_corpus <dir> tests/fixtures/dm32_*.ambe49
    python3 tools/proto_voicing.py <dir>
"""
import sys, glob, os
import numpy as np

FS = 8000
FRAME = 160
Q_F0 = 19


def load(d):
    out = []
    for meta in sorted(glob.glob(os.path.join(d, "*.meta"))):
        pcm = np.fromfile(meta[:-5] + ".pcm", dtype="<i2").astype(np.float64)
        rows = []
        for ln in open(meta):
            p = ln.split()
            rows.append((int(p[0]), int(p[1]), int(p[2]), int(p[3]), p[4]))
        out.append((pcm, rows))
    return out


def ratios(pcm, end, nwin, nfft, f0, L):
    """The library's measure, at an arbitrary window length.

    Identical in form to ambe_analysis.c: Hamming window, |X|^2, then per
    harmonic a numerator over +-step/4 of its centre and a denominator over
    +-step/2, accumulated into the 8 voicing bands.  Only nwin changes.
    """
    if end - nwin < 0:
        return None
    x = pcm[end - nwin:end] * np.hamming(nwin)
    X = np.abs(np.fft.rfft(x, nfft)) ** 2
    bph = f0 * nfft                       # bins per harmonic
    tot = np.zeros(8)
    pk = np.zeros(8)
    for l in range(1, L + 1):
        c = l * bph
        lo = int(round(c - bph / 2))
        hi = int(round(c + bph / 2))
        lo = max(lo, 1)
        hi = min(hi, nfft // 2)
        if hi < lo:
            continue
        k = np.arange(lo, hi + 1)
        m = X[k]
        j = min(int(l * 16 * f0), 7)
        tot[j] += m.sum()
        pk[j] += m[np.abs(k - c) <= bph / 4].sum()
    return pk, tot, bph


def score(vals, truth):
    """Best achievable accuracy over all thresholds, and Cohen's kappa there.

    The mean separation is not the decision.  Two distributions can be shifted
    apart and still overlap so heavily that no threshold beats answering with
    the majority class - which is exactly the failure the shipped estimator has
    (48% against a 67% trivial baseline).  So the number that settles whether a
    change is worth transcribing is the best accuracy any threshold can reach,
    measured against that baseline on the same bands.
    """
    v = np.asarray(vals)
    t = np.asarray(truth, dtype=bool)
    n = len(v)
    base = max(t.sum(), n - t.sum()) / n
    order = np.argsort(v)
    ts = t[order]
    # sweep the threshold: predict voiced above it
    voiced_above = np.cumsum(ts[::-1])[::-1]          # voiced with value >= i
    unvoiced_below = np.cumsum(~ts) - (~ts)           # unvoiced with value < i
    correct = voiced_above + unvoiced_below
    best_i = int(np.argmax(correct))
    acc = correct[best_i] / n
    # kappa at that operating point
    pred = np.zeros(n, dtype=bool); pred[best_i:] = True
    tt = ts
    po = (pred == tt).mean()
    pe = (pred.mean() * tt.mean()) + ((1 - pred.mean()) * (1 - tt.mean()))
    kappa = (po - pe) / (1 - pe) if pe < 1 else 0.0
    # AUC: threshold-free, and prior-free.  Accuracy against a 2:1 majority
    # can sit exactly at the baseline while the measure still carries real
    # information, so the two numbers answer different questions - "would a
    # decision built on this beat always-voiced" and "is there anything here
    # at all".
    ranks = np.empty(n, dtype=np.float64)
    ranks[order] = np.arange(1, n + 1)
    nv_, nu_ = t.sum(), n - t.sum()
    auc = ((ranks[t].sum() - nv_ * (nv_ + 1) / 2.0) / (nv_ * nu_)
           if nv_ and nu_ else float('nan'))
    return base, acc, kappa, auc



def synthetic_control(nwin=320, nfft=512):
    """Is the measure itself sound?  Ground truth, no corpus.

    Builds a perfectly voiced signal (pure harmonics) and a perfectly unvoiced
    one synthesised exactly the way ambe_synth.c does it - uvquality = 3 lines
    per harmonic at l-1/3, l, l+1/3 with independent random phases - and runs
    the same measure over both.  If the measure works at all it must score the
    first near 1.0 and the second near 0.33, because two of the three unvoiced
    lines fall outside the +-step/4 numerator window by construction.

    This is what separates "the formula is wrong" from "the signal does not
    carry it", and the answer is the latter: the measure separates these by
    +0.42 to +0.70, far more than it ever manages on real decoded speech.
    """
    rng = np.random.default_rng(1)
    print("\nControl: the measure on synthetic signals, where truth is exact\n")
    print("    f0     bins/harm   voiced   unvoiced   separation")
    print("  ------   ---------   ------   --------   ----------")
    for f0 in (0.0100, 0.0150, 0.0200, 0.0250, 0.0313, 0.0400, 0.0500):
        L = min(int(0.5 / f0), 56)
        n = np.arange(nwin)
        v = np.zeros(nwin)
        u = np.zeros(nwin)
        for l in range(1, L + 1):
            v += np.cos(2 * np.pi * f0 * l * n + rng.uniform(0, 2 * np.pi))
            for i in range(3):
                num = (l * 6 + 2 * i - 2) / 6.0
                u += np.cos(2 * np.pi * f0 * num * n
                            + rng.uniform(0, 2 * np.pi)) / np.sqrt(3)

        def one(x):
            w = x * np.hamming(nwin)
            X = np.abs(np.fft.rfft(w, nfft)) ** 2
            bph = f0 * nfft
            tot = pk = 0.0
            for l in range(1, L + 1):
                c = l * bph
                lo = max(int(round(c - bph / 2)), 1)
                hi = min(int(round(c + bph / 2)), nfft // 2)
                if hi < lo:
                    continue
                k = np.arange(lo, hi + 1)
                m = X[k]
                tot += m.sum()
                pk += m[np.abs(k - c) <= bph / 4].sum()
            return pk / tot if tot > 0 else float('nan')

        rv, ru = one(v), one(u)
        print("  %.4f   %9.2f   %6.3f   %8.3f   %+10.3f"
              % (f0, f0 * nfft, rv, ru, rv - ru))


def main():
    caps = load(sys.argv[1])

    print("\nSweep 1: does resolution move the measure?\n")
    print("window  nfft   bins/harm   voiced   unvoiced   separation   "
          "baseline   best acc   kappa     AUC     n")
    print("------  -----  ---------   ------   --------   ----------   "
          "--------   --------   ------   -----   -----")
    for nwin, nfft in ((199, 256), (199, 512), (320, 512), (464, 512),
                       (464, 1024), (640, 1024), (928, 1024)):
        sv = su = 0.0
        nv = nu = 0
        bph_sum = 0.0
        nb = 0
        allv = []
        allt = []
        for pcm, rows in caps:
            for (idx, f0q, L, b1, vpat) in rows:
                if b1 < 0:
                    continue
                f0 = f0q / float(1 << Q_F0)
                r = ratios(pcm, (idx + 1) * FRAME, nwin, nfft, f0, L)
                if r is None:
                    continue
                pk, tot, bph = r
                bph_sum += bph
                nb += 1
                for j in range(8):
                    if tot[j] <= 0:
                        continue
                    v = pk[j] / tot[j]
                    allv.append(v)
                    allt.append(vpat[j] == '1')
                    if vpat[j] == '1':
                        sv += v; nv += 1
                    else:
                        su += v; nu += 1
        mv = sv / nv if nv else float('nan')
        mu = su / nu if nu else float('nan')
        base, acc, kappa, auc = score(allv, allt)
        print("%6d  %5d  %9.2f   %6.3f   %8.3f   %+10.3f   "
              "%8.3f   %8.3f   %+6.3f   %5.3f   %5d"
              % (nwin, nfft, bph_sum / max(nb, 1), mv, mu, mv - mu,
                 base, acc, kappa, auc, nv + nu))

    print("\nSweep 2: where does the measure carry information, per band?"
          "  (window 320, nfft 512 - the best row above)\n")
    V = [[] for _ in range(8)]
    T = [[] for _ in range(8)]
    for pcm, rows in caps:
        for (idx, f0q, L, b1, vpat) in rows:
            if b1 < 0:
                continue
            f0 = f0q / float(1 << Q_F0)
            r = ratios(pcm, (idx + 1) * FRAME, 320, 512, f0, L)
            if r is None:
                continue
            pk, tot, bph = r
            for j in range(8):
                if tot[j] <= 0:
                    continue
                V[j].append(pk[j] / tot[j])
                T[j].append(vpat[j] == '1')

    print("band     n   %voiced    AUC   baseline  best acc    gain")
    print("----  ----   -------   -----  --------  --------   -----")
    tn = tc = tb = 0
    for j in range(8):
        b, a, k, auc = score(V[j], T[j])
        n = len(V[j])
        print("  %d   %4d    %5.1f%%   %5.3f   %6.3f    %6.3f    %+.3f"
              % (j, n, 100.0 * float(np.mean(T[j])), auc, b, a, a - b))
        tn += n; tc += a * n; tb += b * n
    allt = np.concatenate([np.array(T[j]) for j in range(8)])
    print("\n  always voiced        %.3f   (the corpus is %.1f%% voiced)"
          % (allt.mean(), 100.0 * allt.mean()))
    print("  per-band prior only  %.3f" % (tb / tn))
    print("  per-band + the ratio %.3f   (the ratio is worth %+.3f)"
          % (tc / tn, (tc - tb) / tn))

    synthetic_control()


main()
