/* FormantSynth: the klattsch synthesis engine.
 *
 * Translated from src/engine/synth-core.js. klattsch is Tony Gies's work; a
 * translation of someone's algorithm is still their algorithm.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */

#include "kl_synth.h"

#include <math.h>
#include <string.h>

#define KL_PI 3.141592653589793
#define KL_TWO_PI 6.283185307179586

const double kl_param_default[KL_PARAM_COUNT] = {
    120.0,      /* F0            */
    0.0,        /* voicing       */
    500.0,      /* F1            */
    80.0,       /* BW1           */
    0.0,        /* A1            */
    1500.0,     /* F2            */
    120.0,      /* BW2           */
    0.0,        /* A2            */
    2500.0,     /* F3            */
    160.0,      /* BW3           */
    0.0,        /* A3            */
    3.5,        /* gain          */
    0.0,        /* vibratoDepth  */
    5.0,        /* vibratoRate   */
    0.0,        /* tremoloDepth  */
    5.0,        /* tremoloRate   */
    0.0,        /* aspiration    */
    0.0,        /* tilt          */
    0.5         /* effort        */
};

const char *const kl_param_name[KL_PARAM_COUNT] = {
    "F0", "voicing",
    "F1", "BW1", "A1",
    "F2", "BW2", "A2",
    "F3", "BW3", "A3",
    "gain",
    "vibratoDepth", "vibratoRate",
    "tremoloDepth", "tremoloRate",
    "aspiration", "tilt", "effort"
};

void kl_schedule_prepare(const kl_event *events, size_t len, double sample_rate,
                         long *at_sample, long *transition_len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        /* Math.floor, not truncation: a negative atMs would round the wrong
         * way under a cast, and the JS floors. */
        at_sample[i] = (long)floor(events[i].at_ms * sample_rate / 1000.0);
        {
            const double n = floor(events[i].transition_ms * sample_rate / 1000.0);
            transition_len[i] = n < 1.0 ? 1L : (long)n;   /* Math.max(1, ...) */
        }
    }
}

static void kl_reset_state(kl_synth *s, const double *initial, uint32_t initial_present)
{
    int k;
    s->glottal_phase = 0.0;
    s->vibrato_phase = 0.0;
    s->tremolo_phase = 0.0;
    s->tilt_prev = 0.0;
    s->lfsr = (int32_t)0xACE1ACE1u;
    kl_biquad_init(&s->bp1);
    kl_biquad_init(&s->bp2);
    kl_biquad_init(&s->bp3);

    for (k = 0; k < KL_PARAM_COUNT; k++) {
        /* { ...DEFAULT, ...initialTarget } */
        const double v = (initial && (initial_present & (1u << k)))
                       ? initial[k] : kl_param_default[k];
        s->current[k] = v;
        s->target[k] = v;
        s->increment[k] = 0.0;
    }
    s->transition_samples = 0;
    s->schedule = NULL;
    s->schedule_len = 0;
    s->schedule_idx = 0;
    s->sample_counter = 0;
    s->at_sample = NULL;
    s->transition_len = NULL;
}

void kl_synth_init(kl_synth *s, double sample_rate,
                   const double *initial, uint32_t initial_present)
{
    memset(s, 0, sizeof *s);
    s->sr = sample_rate;
    kl_reset_state(s, initial, initial_present);
}

void kl_synth_reset(kl_synth *s, const double *initial, uint32_t initial_present)
{
    kl_reset_state(s, initial, initial_present);
}

void kl_synth_queue(kl_synth *s, const kl_event *events, size_t len,
                    const long *at_sample, const long *transition_len,
                    long start_counter)
{
    s->schedule = events;
    s->schedule_len = len;
    s->at_sample = at_sample;
    s->transition_len = transition_len;
    s->schedule_idx = 0;
    s->sample_counter = start_counter;
}

/* Apply one event's targets and recompute every increment.
 *
 * Every parameter's increment is recomputed, not only the ones the event
 * carries: a parameter the event omits keeps its previous target but still
 * gets a fresh increment from wherever `current` has drifted to. That is what
 * the JS loop over PARAMS does, and it is the difference between a parameter
 * resuming its glide and jumping. */
static void kl_apply(kl_synth *s, const double *values, uint32_t present, long n)
{
    int k;
    const double dn = (double)n;
    s->transition_samples = n;
    for (k = 0; k < KL_PARAM_COUNT; k++) {
        if (present & (1u << k)) s->target[k] = values[k];
        s->increment[k] = (s->target[k] - s->current[k]) / dn;
    }
}

void kl_synth_set_target(kl_synth *s, const double *values, uint32_t present,
                         double transition_ms)
{
    const double f = floor(transition_ms * s->sr / 1000.0);
    const long n = f < 1.0 ? 1L : (long)f;
    kl_apply(s, values, present, n);
}

void kl_synth_process(kl_synth *s, float *out, size_t n)
{
    double *cur = s->current;
    size_t i;

    for (i = 0; i < n; i++) {
        /* Drain any baked-in schedule events whose time has arrived. */
        while (s->schedule_idx < s->schedule_len
               && s->at_sample[s->schedule_idx] <= s->sample_counter) {
            const size_t idx = s->schedule_idx++;
            kl_apply(s, s->schedule[idx].value, s->schedule[idx].present,
                     s->transition_len[idx]);
        }
        s->sample_counter++;

        if (s->transition_samples > 0) {
            int k;
            for (k = 0; k < KL_PARAM_COUNT; k++) cur[k] += s->increment[k];
            s->transition_samples--;
            if (s->transition_samples == 0) {
                for (k = 0; k < KL_PARAM_COUNT; k++) cur[k] = s->target[k];
            }
        }

        {
            double eff_f0, tremolo_mod, v, noise, pulse, voiced_gain, exc, y, tilted;

            /* Vibrato LFO modulates F0 around its target value. */
            s->vibrato_phase += KL_TWO_PI * cur[KL_VIBRATO_RATE] / s->sr;
            s->vibrato_phase -= KL_TWO_PI * floor(s->vibrato_phase / KL_TWO_PI);
            eff_f0 = cur[KL_F0] + cur[KL_VIBRATO_DEPTH] * sin(s->vibrato_phase);

            /* Tremolo LFO modulates output amplitude. Unipolar: at depth 1 it
             * sweeps 1 down to 0 and never exceeds 1. */
            s->tremolo_phase += KL_TWO_PI * cur[KL_TREMOLO_RATE] / s->sr;
            s->tremolo_phase -= KL_TWO_PI * floor(s->tremolo_phase / KL_TWO_PI);
            tremolo_mod = 1.0 - cur[KL_TREMOLO_DEPTH] * (0.5 + 0.5 * sin(s->tremolo_phase));

            v = cur[KL_VOICING] < 0.0 ? 0.0 : (cur[KL_VOICING] > 1.0 ? 1.0 : cur[KL_VOICING]);
            s->lfsr = kl_xorshift(s->lfsr);
            /* The SIGNED state over 2^31, so the range is [-1, 1). */
            noise = (double)s->lfsr / 2147483648.0;
            pulse = kl_glottal_pulse(s->glottal_phase, cur[KL_EFFORT]);
            voiced_gain = 1.0 - cur[KL_ASPIRATION] * 0.85;
            exc = v * pulse * voiced_gain
                + (1.0 - v) * noise * 0.35
                + cur[KL_ASPIRATION] * noise * 0.5;
            s->glottal_phase += eff_f0 / s->sr;
            s->glottal_phase -= floor(s->glottal_phase);

            kl_biquad_set(&s->bp1, cur[KL_FF1], cur[KL_BW1], s->sr);
            kl_biquad_set(&s->bp2, cur[KL_FF2], cur[KL_BW2], s->sr);
            kl_biquad_set(&s->bp3, cur[KL_FF3], cur[KL_BW3], s->sr);

            y = (kl_biquad_process(&s->bp1, exc) * cur[KL_A1]
               + kl_biquad_process(&s->bp2, exc) * cur[KL_A2]
               + kl_biquad_process(&s->bp3, exc) * cur[KL_A3]) * cur[KL_GAIN] * tremolo_mod;

            /* One-zero spectral tilt. tilt_prev holds the previous *untilted*
             * y, so the filter does not feed back on itself. */
            tilted = y - cur[KL_TILT] * s->tilt_prev;
            s->tilt_prev = y;

            /* The single rounding to float, matching the JS store into a
             * Float32Array. */
            out[i] = (float)kl_soft_clip(tilted);
        }
    }
}
