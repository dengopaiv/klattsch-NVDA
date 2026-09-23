/* FormantSynth: the klattsch synthesis engine, free of any audio-API
 * dependency.
 *
 * Translated from src/engine/synth-core.js. klattsch is Tony Gies's work; a
 * translation of someone's algorithm is still their algorithm.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */
#ifndef KL_SYNTH_H
#define KL_SYNTH_H

#include <stddef.h>
#include <stdint.h>

#include "kl_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The interpolated parameters, in the order src/engine/synth-core.js declares
 * PARAMS. The order is observable: the JS recomputes every increment in this
 * order on each schedule event, and the goldens digest targets in it. Changing
 * it means changing the generator, the dump tool and the verifier together. */
typedef enum {
    KL_F0 = 0,
    KL_VOICING,
    KL_FF1, KL_BW1, KL_A1,
    KL_FF2, KL_BW2, KL_A2,
    KL_FF3, KL_BW3, KL_A3,
    KL_GAIN,
    KL_VIBRATO_DEPTH,
    KL_VIBRATO_RATE,
    KL_TREMOLO_DEPTH,
    KL_TREMOLO_RATE,
    KL_ASPIRATION,
    KL_TILT,
    KL_EFFORT,
    KL_PARAM_COUNT
} kl_param;

/* DEFAULT in synth-core.js, in kl_param order. */
extern const double kl_param_default[KL_PARAM_COUNT];

/* Parameter names, in kl_param order, for tooling. */
extern const char *const kl_param_name[KL_PARAM_COUNT];

/* One scheduled parameter change.
 *
 * `present` is a bitmask over kl_param: a bit set means the event carries a
 * value for that parameter. Absent parameters keep their previous *target*,
 * which is not the same as keeping their current value -- the increment is
 * recomputed for every parameter on every event either way. */
typedef struct {
    double at_ms;
    double transition_ms;
    uint32_t present;
    double value[KL_PARAM_COUNT];
} kl_event;

typedef struct {
    double sr;

    double current[KL_PARAM_COUNT];
    double target[KL_PARAM_COUNT];
    double increment[KL_PARAM_COUNT];
    long transition_samples;

    double glottal_phase;
    double vibrato_phase;
    double tremolo_phase;
    double tilt_prev;
    int32_t lfsr;

    kl_biquad bp1, bp2, bp3;

    /* The baked-in schedule, converted to samples once. Borrowed, not owned:
     * the caller keeps the array alive. Nothing here allocates. */
    const kl_event *schedule;
    size_t schedule_len;
    size_t schedule_idx;
    long   sample_counter;

    /* Per-event sample conversions, parallel to `schedule`. Borrowed too. */
    const long *at_sample;
    const long *transition_len;
} kl_synth;

/* Convert a schedule to sample-domain timings. `at_sample` and
 * `transition_len` must have room for `len` entries and must outlive the
 * synth that uses them. Split out from init because the conversion depends
 * only on the sample rate, so one converted schedule can drive several
 * synths -- which is what the polyphony path will want. */
void kl_schedule_prepare(const kl_event *events, size_t len, double sample_rate,
                         long *at_sample, long *transition_len);

/* Initialise. `initial` may be NULL for the defaults; otherwise it is a
 * KL_PARAM_COUNT array and `initial_present` a mask of which entries to take
 * (the rest come from kl_param_default), matching the JS's
 * `{ ...DEFAULT, ...initialTarget }`. */
void kl_synth_init(kl_synth *s, double sample_rate,
                   const double *initial, uint32_t initial_present);

/* Attach a prepared schedule. `start_counter` presets the clock: negative
 * delays the first event, positive drains already-past events on the first
 * render call. */
void kl_synth_queue(kl_synth *s, const kl_event *events, size_t len,
                    const long *at_sample, const long *transition_len,
                    long start_counter);

/* Schedule a new target directly, interpolated over `transition_ms`. */
void kl_synth_set_target(kl_synth *s, const double *values, uint32_t present,
                         double transition_ms);

/* Reset to the initial state, dropping any schedule. */
void kl_synth_reset(kl_synth *s, const double *initial, uint32_t initial_present);

/* Render `n` samples into `out`.
 *
 * `out` is float, not double: the JavaScript writes into a Float32Array, so
 * the rounding to single precision is part of the algorithm and happens once,
 * on the store. Everything before it is double. The synth keeps its state
 * between calls, so rendering in chunks gives the same samples as rendering
 * whole -- which is what the chunked-rendering work in phase 3 depends on. */
void kl_synth_process(kl_synth *s, float *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* KL_SYNTH_H */
