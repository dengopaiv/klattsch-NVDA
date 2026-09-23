/* Low-level DSP primitives used by the formant synth core.
 *
 * Translated from src/engine/dsp.js. klattsch is Tony Gies's work; a
 * translation of someone's algorithm is still their algorithm.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */

#include "kl_dsp.h"

#include <math.h>

/* The JS uses Math.PI, which is the double nearest pi -- the same value
 * M_PI names. M_PI is not in standard C, so it is spelled out here rather
 * than depending on _USE_MATH_DEFINES or a POSIX extension. */
#define KL_PI 3.141592653589793

void kl_biquad_init(kl_biquad *b)
{
    b->x1 = 0.0; b->x2 = 0.0;
    b->y1 = 0.0; b->y2 = 0.0;
    b->b0 = 0.0; b->b1 = 0.0; b->b2 = 0.0;
    b->a1 = 0.0; b->a2 = 0.0;
    b->last_f = -1.0; b->last_bw = -1.0;
}

void kl_biquad_set(kl_biquad *b, double f, double bw, double sr)
{
    /* Cache check on the raw arguments, before the clamps. See the header. */
    if (f == b->last_f && bw == b->last_bw) return;
    b->last_f = f;
    b->last_bw = bw;

    /* Math.max(40, Math.min(sr * 0.45, f)) -- the inner min first. Written in
     * this order so the NaN behaviour matches: Math.min/Math.max propagate NaN,
     * and so does this, because a NaN comparison is false and the else branch
     * keeps the NaN. */
    {
        const double hi = sr * 0.45;
        if (f > hi) f = hi;
        if (f < 40.0) f = 40.0;
        if (bw < 20.0) bw = 20.0;
    }

    {
        const double w0 = 2.0 * KL_PI * f / sr;
        const double cosw0 = cos(w0);
        const double sinw0 = sin(w0);
        const double Q = f / bw;
        const double alpha = sinw0 / (2.0 * Q);
        const double a0 = 1.0 + alpha;
        b->b0 =  alpha / a0;
        b->b1 =  0.0;
        b->b2 = -alpha / a0;
        b->a1 = -2.0 * cosw0 / a0;
        b->a2 = (1.0 - alpha) / a0;
    }
}

double kl_biquad_process(kl_biquad *b, double x)
{
    const double y = b->b0 * x + b->b1 * b->x1 + b->b2 * b->x2
                   - b->a1 * b->y1 - b->a2 * b->y2;
    b->x2 = b->x1; b->x1 = x;
    b->y2 = b->y1; b->y1 = y;
    return y;
}

void kl_biquad_reset(kl_biquad *b)
{
    b->x1 = 0.0; b->x2 = 0.0;
    b->y1 = 0.0; b->y2 = 0.0;
}

double kl_glottal_pulse(double phase, double effort)
{
    /* JS: effort < 0 ? 0 : effort > 1 ? 1 : effort. A NaN effort falls through
     * both comparisons and stays NaN, which this reproduces. */
    const double e = effort < 0.0 ? 0.0 : (effort > 1.0 ? 1.0 : effort);
    const double Tp = 0.5 - e * 0.2;    /* 0.5 (lax) -> 0.3 (tense) */
    const double Tn = 0.25 - e * 0.17;  /* 0.25 (lax) -> 0.08 (tense) */
    const double NORM = 0.1;

    if (phase < Tp) {
        return NORM * 0.5 * (KL_PI / Tp) * sin(KL_PI * phase / Tp);
    }
    if (phase < Tp + Tn) {
        return -NORM * (KL_PI / (2.0 * Tn)) * sin(KL_PI * (phase - Tp) / (2.0 * Tn));
    }
    return 0.0;
}

int32_t kl_xorshift(int32_t state)
{
    /* Shifts on the unsigned reinterpretation: `x ^= x >>> 17` in JS is a
     * logical shift, and the left shifts wrap rather than overflowing (signed
     * left-shift overflow is undefined in C, so it must be done unsigned). */
    uint32_t x = (uint32_t)state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    /* Back to signed. Implementation-defined before C20 for values above
     * INT32_MAX, and two's complement on every target here; C23 makes it
     * well-defined. The cast is what the caller needs: the sample is the
     * SIGNED state over 2^31. */
    return (int32_t)x;
}

double kl_soft_clip(double x)
{
    const double T = 0.85;
    const double a = x < 0.0 ? -x : x;
    if (a <= T) return x;
    {
        const double sign = x < 0.0 ? -1.0 : 1.0;
        const double excess = a - T;
        return sign * (T + (1.0 - T) * excess / (excess + 1.0));
    }
}
