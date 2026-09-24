/* Render a compiled utterance: every voice section to its own buffer, summed.
 *
 * Translated from bin/klattsch.mjs. klattsch is Tony Gies's work; a
 * translation of someone's algorithm is still their algorithm.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */

#include "kl_render.h"

#include <math.h>
#include <string.h>

size_t kl_render_samples_for(double ms, double sample_rate)
{
    double n = ceil(ms * sample_rate / 1000.0);
    if (!(n > 0.0)) return 0;
    return (size_t)n;
}

size_t kl_render_need_sched(const kl_compiled *c)
{
    size_t i, most = 0;
    for (i = 0; i < c->n_voices; i++)
        if (c->voices[i].n_events > most) most = c->voices[i].n_events;
    return most;
}

int kl_render_mix(const kl_compiled *c, double sample_rate,
                  float *out, size_t out_n, const kl_render_arena *a)
{
    size_t vi;

    if (a->scratch_cap < out_n) return KL_RENDER_OVERFLOW;
    if (a->sched_cap < kl_render_need_sched(c)) return KL_RENDER_OVERFLOW;

    memset(out, 0, out_n * sizeof *out);

    for (vi = 0; vi < c->n_voices; vi++) {
        const kl_voice *v = &c->voices[vi];
        size_t vn, n, i;
        kl_synth s;

        /* `if (!v.schedule.length) continue` in the JavaScript. Unreached by
         * the corpus -- every section emits at least one event, because the
         * compiler always closes a section with a trail-off -- and kept
         * because the JavaScript keeps it. See docs/18-stage6-wav.md. */
        if (v->n_events == 0) continue;

        vn = kl_render_samples_for(v->total_ms, sample_rate);
        if (vn > a->scratch_cap) return KL_RENDER_OVERFLOW;

        kl_schedule_prepare(v->events, v->n_events, sample_rate,
                            a->at_sample, a->transition_len);
        kl_synth_init(&s, sample_rate, NULL, 0);
        kl_synth_queue(&s, v->events, v->n_events,
                       a->at_sample, a->transition_len, 0);
        kl_synth_process(&s, a->scratch, vn);

        n = vn < out_n ? vn : out_n;
        for (i = 0; i < n; i++) out[i] += a->scratch[i];
    }

    return KL_RENDER_OK;
}
