/* Minimal test scaffolding: no framework, no dependencies. */
#ifndef TESTUTIL_H
#define TESTUTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdint.h>

static int t_fail;
static int t_checks;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        t_checks++;                                                           \
        if (!(cond)) {                                                        \
            if (t_fail < 10) {                                                \
                fprintf(stderr, "\n  FAIL %s:%d: ", __FILE__, __LINE__);      \
                fprintf(stderr, __VA_ARGS__);                                 \
            }                                                                 \
            t_fail++;                                                         \
        }                                                                     \
    } while (0)

static inline int t_done(const char *what)
{
    if (t_fail) {
        fprintf(stderr, "\n  %s: %d of %d checks failed\n", what, t_fail, t_checks);
        return 1;
    }
    printf("ok   %d checks   %s\n", t_checks, what);
    return 0;
}

static inline const char *fixture(const char *name)
{
    static char buf[512];
    const char *dir = getenv("AMBE_FIXTURES");
    snprintf(buf, sizeof(buf), "%s/%s", dir ? dir : "tests/fixtures", name);
    return buf;
}

static inline FILE *fixture_open(const char *name)
{
    const char *p = fixture(name);
    FILE *f = fopen(p, "rb");
    if (!f) {
        fprintf(stderr, "\n  cannot open fixture %s: %s\n", p, strerror(errno));
        exit(2);
    }
    return f;
}

static inline int hexnib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Pearson correlation of two equal-length signals. */
static inline double correlation(const short *a, const short *b, int n)
{
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, den;
    int i;
    for (i = 0; i < n; i++) {
        sa += a[i]; sb += b[i];
        saa += (double)a[i] * a[i];
        sbb += (double)b[i] * b[i];
        sab += (double)a[i] * b[i];
    }
    den = sqrt((saa - sa * sa / n) * (sbb - sb * sb / n));
    if (den <= 0.0)
        return 1.0;
    return (sab - sa * sb / n) / den;
}

static inline double rms(const short *a, int n)
{
    double s = 0;
    int i;
    for (i = 0; i < n; i++)
        s += (double)a[i] * a[i];
    return sqrt(s / n);
}

/*
 * Coarse magnitude spectrum: 16 bands over the 0..4 kHz range of one 20 ms
 * frame.  MBE synthesis randomises the phase of every harmonic above L/4, so
 * two correct implementations produce different waveforms with the same
 * spectrum; band energies are the right thing to compare.
 */
#define T_BANDS 16

static inline void band_energies(const short *x, int n, double *out)
{
    int b, k, i;
    for (b = 0; b < T_BANDS; b++)
        out[b] = 0.0;
    for (k = 1; k <= T_BANDS * 5; k++) {
        double re = 0.0, im = 0.0, w = 2.0 * M_PI * k / n;
        for (i = 0; i < n; i++) {
            re += x[i] * cos(w * i);
            im -= x[i] * sin(w * i);
        }
        out[(k - 1) / 5] += re * re + im * im;
    }
    for (b = 0; b < T_BANDS; b++)
        out[b] = log10(out[b] + 1.0);
}

static inline double dcorrelation(const double *a, const double *b, int n)
{
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, den;
    int i;
    for (i = 0; i < n; i++) {
        sa += a[i]; sb += b[i];
        saa += a[i] * a[i];
        sbb += b[i] * b[i];
        sab += a[i] * b[i];
    }
    den = sqrt((saa - sa * sa / n) * (sbb - sb * sb / n));
    if (den <= 0.0)
        return 1.0;
    return (sab - sa * sb / n) / den;
}

static inline uint32_t fnv1a(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

#endif
