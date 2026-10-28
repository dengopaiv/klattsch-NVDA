/* kl_stream.c -- kl_render_mix(), one chunk at a time. See kl_stream.h.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */
#include <string.h>

#include "kl_render.h"
#include "kl_stream.h"

size_t kl_stream_need_events(const kl_compiled *c)
{
    size_t i, n = 0;
    for (i = 0; i < c->n_voices; i++)
        n += c->voices[i].n_events;
    return n;
}

size_t kl_stream_total(const kl_compiled *c, double sample_rate)
{
    return kl_render_samples_for(c->total_ms, sample_rate);
}

void kl_stream_start(kl_stream *s, const kl_compiled *c, double sample_rate)
{
    size_t vi, off = 0;

    s->c = c;
    s->sr = sample_rate;
    s->pos = 0;
    s->total = kl_stream_total(c, sample_rate);

    for (vi = 0; vi < c->n_voices; vi++) {
        const kl_voice *v = &c->voices[vi];

        /* As in kl_render_mix: a section with no events is skipped. Its
         * length is 0 here, so the render loop never touches its synth. */
        if (v->n_events == 0) {
            s->voice_n[vi] = 0;
            continue;
        }
        s->voice_n[vi] = kl_render_samples_for(v->total_ms, sample_rate);

        kl_schedule_prepare(v->events, v->n_events, sample_rate,
                            s->at_sample + off, s->transition_len + off);
        kl_synth_init(&s->synth[vi], sample_rate, NULL, 0);
        kl_synth_queue(&s->synth[vi], v->events, v->n_events,
                       s->at_sample + off, s->transition_len + off, 0);
        off += v->n_events;
    }
}

size_t kl_stream_render(kl_stream *s, float *out, size_t n)
{
    size_t vi, i;

    if (n > s->total - s->pos) n = s->total - s->pos;
    if (n > s->vbuf_cap) n = s->vbuf_cap;
    if (n == 0) return 0;

    /* kl_render_mix starts from a zeroed buffer and adds voice 0 into it, so
     * the first add is 0.0f + x here too. */
    memset(out, 0, n * sizeof *out);

    for (vi = 0; vi < s->c->n_voices; vi++) {
        size_t vn = s->voice_n[vi], m;
        if (s->pos >= vn) continue;
        m = vn - s->pos < n ? vn - s->pos : n;
        kl_synth_process(&s->synth[vi], s->vbuf, m);
        for (i = 0; i < m; i++) out[i] += s->vbuf[i];
    }

    s->pos += n;
    return n;
}
