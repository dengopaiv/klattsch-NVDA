/* Render a compiled utterance: every voice section to its own buffer, summed.
 *
 * Translated from bin/klattsch.mjs, which is the only place upstream puts
 * these six lines. klattsch is Tony Gies's work; a translation of someone's
 * algorithm is still their algorithm.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 *
 * Two details here are observable and neither is arbitrary:
 *
 *  - Each voice is rendered into a float buffer and then *added* to the mix
 *    in float. The JavaScript sums Float32Arrays, so the rounding to single
 *    precision happens once per voice per sample, not once at the end. A
 *    double accumulator would be more accurate and would not be the same
 *    file.
 *  - The mix is as long as the longest section, each voice is as long as its
 *    own totalMs, and the add stops at the shorter of the two. A section can
 *    be shorter than the utterance -- `voice/leading-marker` is -- so the
 *    guard is reached rather than defensive.
 */
#ifndef KL_RENDER_H
#define KL_RENDER_H

#include <stddef.h>

#include "kl_compile.h"
#include "kl_synth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Math.ceil(ms * sampleRate / 1000), in the JavaScript's operand order: the
 * multiply happens before the divide, and doing it the other way changes the
 * last bit often enough to change a sample count. */
size_t kl_render_samples_for(double ms, double sample_rate);

/* Caller-provided storage, as everywhere else in this library.
 *
 * `scratch` holds one voice at a time and needs kl_render_samples_for() of
 * the whole utterance; `at_sample` and `transition_len` hold one voice's
 * schedule converted to samples and need kl_render_need_sched() entries. */
typedef struct {
    float *scratch;         size_t scratch_cap;
    long  *at_sample;
    long  *transition_len;  size_t sched_cap;
} kl_render_arena;

/* Events in the largest voice section. */
size_t kl_render_need_sched(const kl_compiled *c);

#define KL_RENDER_OK        0
#define KL_RENDER_OVERFLOW (-1)

/* Render and mix into `out`, which must hold
 * kl_render_samples_for(c->total_ms, sample_rate) samples. `out` is zeroed
 * first, because the JavaScript starts from a fresh Float32Array. */
int kl_render_mix(const kl_compiled *c, double sample_rate,
                  float *out, size_t out_n, const kl_render_arena *a);

#ifdef __cplusplus
}
#endif

#endif /* KL_RENDER_H */
