/* Low-level DSP primitives used by the formant synth core.
 *
 * Translated from src/engine/dsp.js. klattsch is Tony Gies's work; a
 * translation of someone's algorithm is still their algorithm.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 *
 * Everything here computes in double, because the JavaScript it comes from
 * does: JS arithmetic is float64 throughout and only the store into a
 * Float32Array rounds. A float accumulator anywhere in this file would drift
 * away from the reference over a long utterance.
 */
#ifndef KL_DSP_H
#define KL_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Constant-skirt-gain bandpass biquad (RBJ Audio EQ Cookbook).
 *
 * Coefficients are recomputed only when frequency or bandwidth changes. The
 * cache keys on the *raw* arguments, before clamping -- see kl_biquad_set. */
typedef struct {
    double x1, x2;
    double y1, y2;
    double b0, b1, b2;
    double a1, a2;
    double last_f, last_bw;
} kl_biquad;

/* Zero the state and invalidate the coefficient cache. Matches the JS
 * constructor, which starts last_f/last_bw at -1. */
void kl_biquad_init(kl_biquad *b);

/* Recompute coefficients for (f, bw) at sample rate sr.
 *
 * Returns early when f and bw are unchanged since the last call, and that
 * early-out is checked BEFORE the clamps are applied -- so two different raw
 * frequencies that clamp to the same value still each recompute, and the cache
 * hits only on identical raw arguments. The JS does exactly this, and a port
 * that clamps first is observably different at the clamp boundaries. */
void kl_biquad_set(kl_biquad *b, double f, double bw, double sr);

/* One sample through the filter. */
double kl_biquad_process(kl_biquad *b, double x);

/* Zero the delay line, leaving coefficients and the cache alone. */
void kl_biquad_reset(kl_biquad *b);

/* Derivative of the Rosenberg glottal pulse. Phase normalized to [0, 1).
 *
 * `effort` (0..1, clamped) controls the pulse shape: 0 is lax/breathy (longer
 * Tp, gentler closure), 1 is tense (shorter Tp, sharper closure). Peak |value|
 * before normalization is pi / (2*Tn), so the result is divided by 10 to keep
 * amplitude near unity. */
double kl_glottal_pulse(double phase, double effort);

/* 32-bit xorshift LFSR.
 *
 * The shifts are done on the unsigned value -- the JS `x >>> 17` is a logical
 * shift on the unsigned reinterpretation, while `<< 13` and `<< 5` wrap -- and
 * the result is returned signed, because the caller divides the SIGNED state
 * by 2^31 to get a sample in [-1, 1). Returning unsigned here would give noise
 * in [0, 2) and a DC offset through every fricative. */
int32_t kl_xorshift(int32_t state);

/* Soft-clip with a linear region up to +/-0.85 and a smooth knee. */
double kl_soft_clip(double x);

#ifdef __cplusplus
}
#endif

#endif /* KL_DSP_H */
