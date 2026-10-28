/* kl_stream.h -- kl_render_mix(), one chunk at a time.
 *
 * kl_render_mix renders each voice section whole, one after the other, and
 * adds each into the mix in float. Here every voice has its own kl_synth,
 * all of them advance together, and each chunk is summed in the same order
 * with the same float adds. A voice stops contributing at its own length, as
 * it does there -- it is skipped, not added as zeros, because 0.0f + -0.0f
 * is not -0.0f. So the concatenated chunks are kl_render_mix's output,
 * sample for sample and bit for bit, for any chunk sizes: tools' api-stream
 * test holds it to that over the whole corpus.
 *
 * Internal to the library; the add-on reaches it through klattsch_api.h.
 * Storage is the caller's, as everywhere else in this port.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */
#ifndef KL_STREAM_H
#define KL_STREAM_H

#include <stddef.h>

#include "kl_compile.h"
#include "kl_synth.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const kl_compiled *c;
    double sr;

    /* Caller's storage: one synth and one length per voice section, and the
     * sample-domain schedule of every voice side by side, because all of them
     * are live at once (kl_render_mix reuses one array voice after voice). */
    kl_synth *synth;          /* n_voices */
    size_t   *voice_n;        /* n_voices */
    long     *at_sample;      /* kl_stream_need_events() */
    long     *transition_len; /* kl_stream_need_events() */
    float    *vbuf;           /* one voice's chunk */
    size_t    vbuf_cap;

    size_t pos, total;
} kl_stream;

/* Schedule entries over every voice section. */
size_t kl_stream_need_events(const kl_compiled *c);

/* Samples in the whole mix: kl_render_samples_for(c->total_ms, sr). */
size_t kl_stream_total(const kl_compiled *c, double sample_rate);

/* Set up from a compiled utterance; the storage fields must already be set.
 * `c` is borrowed and must outlive the stream. */
void kl_stream_start(kl_stream *s, const kl_compiled *c, double sample_rate);

/* Render the next min(n, vbuf_cap, samples left) samples of the mix into
 * `out`, and return how many. 0 at the end. */
size_t kl_stream_render(kl_stream *s, float *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* KL_STREAM_H */
