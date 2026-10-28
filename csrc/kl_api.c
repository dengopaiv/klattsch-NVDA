/* kl_api.c -- the streaming API. See klattsch_api.h.
 *
 * Everything here is plumbing between pieces that are verified elsewhere:
 * the front end (kl_text), the tokenizer, the compiler, and the chunked mix
 * (kl_stream). The two things it decides for itself are where an index
 * falls and what the level is, and both are written down in
 * docs/21-stage-api.md.
 *
 * Memory: the handle owns its buffers and grows them when an utterance needs
 * more. They never shrink, so once a handle has spoken a long utterance it
 * allocates nothing for shorter ones.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies (the engine this drives)
 * Copyright (c) 2026 Päiv Dengo (this API)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "klattsch_api.h"
#include "kl_banks.h"
#include "kl_compile.h"
#include "kl_render.h"
#include "kl_stream.h"
#include "kl_text.h"
#include "kl_token.h"
#include "kl_version.h"
#include "kl_wav.h"

/* The fixed output gain. Measured over the text corpus so that the add-on's
 * typical level is the generator's: docs/21-stage-api.md §21.4. */
#define KL_API_OUTPUT_GAIN 1.0

/* The longest chunk rendered in one step; a caller asking for more gets this
 * much and asks again. */
#define KL_API_CHUNK 4096

enum { ST_IDLE = 0, ST_BUILDING, ST_RENDERING, ST_DONE };

typedef struct {
    int    id;
    size_t units;    /* UTF-16 offset in the normalized source */
    size_t sample;
} api_index;

struct kl_api {
    double sr;
    double set[KL_SET_COUNT];
    char   bank[64];

    int state;
    int cancelled;

    kl_text_ctx *tx;

    /* The utterance's source, pieces joined by '\n' -- whitespace to the
     * tokenizer, and the end of a `#` comment, so no piece can swallow the
     * next. `units` is its length in the tokenizer's terms: UTF-16 units of
     * the normalized text. */
    char  *src;    size_t src_len, src_cap;
    size_t units;
    char  *piece;  size_t piece_cap;

    api_index *idx; size_t n_idx, idx_cap, idx_next;

    /* Tokenizer and compiler storage, grown to kl_token_need / kl_compile_need. */
    kl_token_list L;
    kl_compile_arena A;
    kl_compiled C;
    int compiled;
    long sounding;
    char *warn; size_t warn_cap;

    size_t spans_cap;        /* A.spans holds A.events_cap entries */

    /* The chunked mix. */
    kl_stream S;
    size_t synth_cap, voice_n_cap, at_cap, trans_cap;
    float vbuf[KL_API_CHUNK];   /* one voice's chunk, inside kl_stream */
    float fbuf[KL_API_CHUNK];   /* the mix, before the level            */
};

static const double DEFAULTS[KL_SET_COUNT] = {
    KL_DEFAULT_BASE_F0,   /* base F0       */
    KL_DEFAULT_RATE,      /* rate          */
    1.0,                  /* scale         */
    0.0, 5.0,             /* vibrato       */
    0.0, 5.0,             /* tremolo       */
    0.0,                  /* aspiration    */
    0.0,                  /* tilt          */
    0.5,                  /* effort        */
    1.0,                  /* inflection    */
    1.0,                  /* volume        */
    0.0,                  /* comma pause   */
};

/* ------------------------------------------------------------- storage -- */

/* `p` grown to hold `need` elements of `size` bytes, doubling. On failure
 * `p` comes back unchanged and *err is set, so nothing leaks and the old
 * contents stay valid. */
static void *grow(void *p, size_t *cap, size_t need, size_t size, int *err)
{
    void *q;
    size_t n;
    if (need <= *cap && p) return p;
    n = *cap ? *cap : 16;
    while (n < need) n *= 2;
    q = realloc(p, n * size);
    if (!q) {
        *err = 1;
        return p;
    }
    *cap = n;
    return q;
}

/* Nonzero on failure. Needs an `int err` in scope. */
#define GROW(ptr, cap, need)     (err = 0, (ptr) = grow((ptr), &(cap), (need), sizeof *(ptr), &err), err)

const char *kl_api_version(void)
{
    return KL_VERSION_STRING;
}

double kl_api_output_gain(void)
{
    return KL_API_OUTPUT_GAIN;
}

kl_api *kl_api_create(int sample_rate)
{
    kl_api *h;
    if (sample_rate <= 0) return NULL;
    h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->tx = calloc(1, sizeof *h->tx);
    if (!h->tx) {
        free(h);
        return NULL;
    }
    h->sr = (double)sample_rate;
    memcpy(h->set, DEFAULTS, sizeof DEFAULTS);
    return h;
}

void kl_api_destroy(kl_api *h)
{
    if (!h) return;
    free(h->tx);
    free(h->src);
    free(h->piece);
    free(h->idx);
    free(h->L.source);
    free(h->L.tokens);
    free(h->L.arena);
    free(h->A.voices);
    free(h->A.events);
    free(h->A.spans);
    free(h->A.extras);
    free(h->A.phrases);
    free(h->A.warnings);
    free(h->A.text);
    free(h->A.live);
    free((void *)h->A.syl);
    free(h->warn);
    free(h->S.synth);
    free(h->S.voice_n);
    free(h->S.at_sample);
    free(h->S.transition_len);
    free(h);
}

int kl_api_sample_rate(const kl_api *h)
{
    return h ? (int)h->sr : 0;
}

/* ------------------------------------------------------------ settings -- */

int kl_api_set(kl_api *h, int setting, double value)
{
    if (!h || setting < 0 || setting >= KL_SET_COUNT || !isfinite(value))
        return KL_API_BAD_ARG;
    switch (setting) {
    case KL_SET_BASE_F0:
    case KL_SET_RATE:
    case KL_SET_SCALE:
        if (value <= 0.0) return KL_API_BAD_ARG;
        break;
    case KL_SET_INFLECTION:
    case KL_SET_VOLUME:
    case KL_SET_COMMA_MS:
        if (value < 0.0) return KL_API_BAD_ARG;
        break;
    default:
        break;
    }
    h->set[setting] = value;
    return KL_API_OK;
}

double kl_api_get(const kl_api *h, int setting)
{
    if (!h || setting < 0 || setting >= KL_SET_COUNT) return NAN;
    return h->set[setting];
}

int kl_api_set_bank(kl_api *h, const char *name)
{
    if (!h) return KL_API_BAD_ARG;
    if (!name || !*name) {
        h->bank[0] = '\0';
        return KL_API_OK;
    }
    if (strlen(name) >= sizeof h->bank || !kl_bank_get(name))
        return KL_API_BAD_BANK;
    strcpy(h->bank, name);
    return KL_API_OK;
}

/* ------------------------------------------------------------- building -- */

int kl_api_begin(kl_api *h)
{
    if (!h) return KL_API_BAD_ARG;
    h->state = ST_BUILDING;
    h->cancelled = 0;
    h->src_len = 0;
    h->units = 0;
    h->n_idx = 0;
    h->idx_next = 0;
    h->compiled = 0;
    h->sounding = 0;
    if (h->warn) h->warn[0] = '\0';
    return KL_API_OK;
}

/* The tokenizer's length for a piece: UTF-16 units after its normalization.
 * ASCII is its own normalization (NFKC, the homoglyph map and the zero-width
 * strip all leave it alone), so only a piece with other bytes in it is
 * measured, by tokenizing it alone. Pieces are joined by '\n', across which
 * NFKC composes nothing, so the lengths add. */
static long piece_units(const char *s, size_t n)
{
    size_t i, ns, nt, na;
    kl_token_list T;
    long r;

    for (i = 0; i < n; i++)
        if ((unsigned char)s[i] >= 0x80) break;
    if (i == n) return (long)n;

    kl_token_need(n, &ns, &nt, &na);
    memset(&T, 0, sizeof T);
    T.source = malloc(ns * sizeof *T.source);
    T.tokens = malloc(nt * sizeof *T.tokens);
    T.arena  = malloc(na ? na : 1);
    T.source_cap = ns;
    T.tokens_cap = nt;
    T.arena_cap = na;
    if (!T.source || !T.tokens || !T.arena)
        r = KL_API_NO_MEMORY;
    else if (kl_tokenize(s, n, &T) != KL_TOKEN_OK)
        r = KL_API_FAILED;
    else
        r = (long)T.source_len;
    free(T.source);
    free(T.tokens);
    free(T.arena);
    return r;
}

/* Append one piece of source. Empty pieces add nothing, not even the
 * separator, so an index after one lands where it would have anyway. */
static int append(kl_api *h, const char *s, size_t n)
{
    long u;
    size_t sep;
    int err;
    if (!h || h->state != ST_BUILDING) return KL_API_BAD_STATE;
    if (n == 0) return KL_API_OK;
    u = piece_units(s, n);
    if (u < 0) return (int)u;
    sep = h->src_len ? 1u : 0u;
    if (GROW(h->src, h->src_cap, h->src_len + sep + n + 1)) return KL_API_NO_MEMORY;
    if (sep) h->src[h->src_len++] = '\n';
    memcpy(h->src + h->src_len, s, n);
    h->src_len += n;
    h->src[h->src_len] = '\0';
    h->units += sep + (size_t)u;
    return KL_API_OK;
}

static kl_text_opts text_opts(const kl_api *h)
{
    kl_text_opts o;
    memset(&o, 0, sizeof o);
    o.base_f0 = h->set[KL_SET_BASE_F0];
    o.comma_ms = (int)(h->set[KL_SET_COMMA_MS] + 0.5);
    o.inflection = h->set[KL_SET_INFLECTION];
    o.set_inflection = 1;
    return o;
}

/* Run a front-end call into h->piece, growing it to the length the call
 * reports, then append the result. */
static int front_end(kl_api *h, const char *utf8, int spell)
{
    kl_text_opts o;
    size_t need;
    int err;
    if (!h || h->state != ST_BUILDING) return KL_API_BAD_STATE;
    if (!utf8) return KL_API_BAD_ARG;
    o = text_opts(h);
    if (GROW(h->piece, h->piece_cap, 256)) return KL_API_NO_MEMORY;
    for (;;) {
        need = spell ? kl_text_spell(h->tx, utf8, h->piece, h->piece_cap)
                     : kl_text_to_source(h->tx, utf8, &o, h->piece, h->piece_cap);
        if (need < h->piece_cap) break;
        if (GROW(h->piece, h->piece_cap, need + 1)) return KL_API_NO_MEMORY;
    }
    return append(h, h->piece, need);
}

int kl_api_text(kl_api *h, const char *utf8)
{
    return front_end(h, utf8, 0);
}

int kl_api_spell(kl_api *h, const char *utf8)
{
    return front_end(h, utf8, 1);
}

int kl_api_source(kl_api *h, const char *utf8)
{
    if (!utf8) return KL_API_BAD_ARG;
    return append(h, utf8, strlen(utf8));
}

int kl_api_break(kl_api *h, int ms)
{
    char tok[16];
    if (!h || h->state != ST_BUILDING) return KL_API_BAD_STATE;
    /* A break of 0 is skipped rather than written as `p0`. That is tidiness,
     * not necessity: `p0` emits a silence event, but always at the same
     * sample as the next event -- a phoneme's, or at the latest the final
     * fade -- whose targets cover every amplitude, so it never sounds.
     * Believed otherwise until tools/api-mutations.py wrote `p0` and the
     * audio did not change; docs/21-stage-api.md §21.5. */
    if (ms <= 0) return KL_API_OK;
    snprintf(tok, sizeof tok, "p%d", ms);
    return append(h, tok, strlen(tok));
}

int kl_api_index(kl_api *h, int index)
{
    int err;
    if (!h || h->state != ST_BUILDING) return KL_API_BAD_STATE;
    if (index == -1) return KL_API_BAD_ARG;
    if (GROW(h->idx, h->idx_cap, h->n_idx + 1)) return KL_API_NO_MEMORY;
    h->idx[h->n_idx].id = index;
    h->idx[h->n_idx].units = h->units;
    h->idx[h->n_idx].sample = 0;
    h->n_idx++;
    return KL_API_OK;
}

/* ------------------------------------------------------------ compiling -- */

static int compile_arena(kl_api *h, const kl_compile_opts *opts)
{
    kl_compile_arena need;
    kl_compile_arena *A = &h->A;
    int err;
    kl_compile_need(&h->L, opts, &need);
    if (GROW(A->voices, A->voices_cap, need.voices_cap)) return -1;
    if (GROW(A->events, A->events_cap, need.events_cap)) return -1;
    if (GROW(A->spans, h->spans_cap, A->events_cap)) return -1;
    if (GROW(A->extras, A->extras_cap, need.extras_cap)) return -1;
    if (GROW(A->phrases, A->phrases_cap, need.phrases_cap)) return -1;
    if (GROW(A->warnings, A->warnings_cap, need.warnings_cap)) return -1;
    if (GROW(A->text, A->text_cap, need.text_cap)) return -1;
    if (GROW(A->live, A->live_cap, need.live_cap)) return -1;
    if (GROW(A->syl, A->syl_cap, need.syl_cap)) return -1;
    return 0;
}

/* Phonemes that make a sound: phoneme phrases other than the silence `_`,
 * less the ones the compiler warned were unknown -- it gives exactly one
 * phrase and one warning to each, and no time. */
static long count_sounding(const kl_compiled *c, int *has_pause)
{
    static const char UNKNOWN[] = "unknown phoneme: ";
    const size_t ul = sizeof UNKNOWN - 1;
    size_t vi, k;
    long n = 0;
    *has_pause = 0;
    for (vi = 0; vi < c->n_voices; vi++) {
        const kl_voice *v = &c->voices[vi];
        for (k = 0; k < v->n_phrases; k++) {
            const kl_phrase *p = &v->phrases[k];
            if (p->kind == KL_TOK_PHONEME) {
                if (p->phoneme_len == 1 && p->phoneme[0] == '_') *has_pause = 1;
                else n++;
            } else {
                *has_pause = 1;   /* a pause, or a `p` directive */
            }
        }
        for (k = 0; k < v->n_warnings; k++) {
            const kl_warning *w = &v->warnings[k];
            if (w->len > ul && memcmp(w->text, UNKNOWN, ul) == 0 &&
                !(w->len == ul + 1 && w->text[ul] == '_'))
                n--;
        }
    }
    return n;
}

static int collect_warnings(kl_api *h)
{
    size_t vi, k, len = 0;
    int err;
    for (vi = 0; vi < h->C.n_voices; vi++)
        for (k = 0; k < h->C.voices[vi].n_warnings; k++)
            len += h->C.voices[vi].warnings[k].len + 2;
    if (GROW(h->warn, h->warn_cap, len + 1)) return -1;
    len = 0;
    for (vi = 0; vi < h->C.n_voices; vi++) {
        for (k = 0; k < h->C.voices[vi].n_warnings; k++) {
            const kl_warning *w = &h->C.voices[vi].warnings[k];
            if (len) {
                h->warn[len++] = ',';
                h->warn[len++] = ' ';
            }
            memcpy(h->warn + len, w->text, w->len);
            len += w->len;
        }
    }
    h->warn[len] = '\0';
    return 0;
}

/* Where each index falls. An index sits at a source offset; it fires when
 * the last token ending at or before that offset has finished sounding --
 * that token's phrase's t_end_ms, which the compiler knew before a sample
 * was rendered. No phrase before it: the start. The final phrase's end is
 * the end of the utterance, trail-off included, so an index after the last
 * word fires when the audio ends. Both lists are in source order, so one
 * pass places them all. */
static void place_indexes(kl_api *h)
{
    const kl_voice *v = (h->compiled && h->C.n_voices) ? &h->C.voices[0] : NULL;
    size_t i, k = 0;
    double t = 0.0;
    for (i = 0; i < h->n_idx; i++) {
        size_t s;
        if (v) {
            while (k < v->n_phrases &&
                   (size_t)v->phrases[k].src_end <= h->idx[i].units) {
                t = v->phrases[k].t_end_ms;
                k++;
            }
        }
        s = kl_render_samples_for(t, h->sr);
        h->idx[i].sample = s < h->S.total ? s : h->S.total;
    }
}

/* A failed or empty compile still owes the caller its indexes: they all
 * come out, at the start, with no audio. */
static long no_audio(kl_api *h, long code)
{
    h->state = ST_RENDERING;
    h->S.total = 0;
    h->S.pos = 0;
    if (code < 0) h->compiled = 0;
    place_indexes(h);
    return code;
}

long kl_api_end(kl_api *h)
{
    kl_compile_opts opts;
    size_t ns, nt, na, ne;
    int has_pause = 0, i, err;

    if (!h || h->state != ST_BUILDING) return KL_API_BAD_STATE;
    h->compiled = 0;

    if (h->src_len == 0)
        return no_audio(h, 0);

    /* Tokenize. */
    kl_token_need(h->src_len, &ns, &nt, &na);
    if (GROW(h->L.source, h->L.source_cap, ns) ||
        GROW(h->L.tokens, h->L.tokens_cap, nt) ||
        GROW(h->L.arena, h->L.arena_cap, na ? na : 1))
        return no_audio(h, KL_API_NO_MEMORY);
    if (kl_tokenize(h->src, h->src_len, &h->L) != KL_TOKEN_OK)
        return no_audio(h, KL_API_FAILED);

    /* Compile, with every option present: "absent" and "the default" are the
     * same to the compiler, so this changes nothing when a setting is left
     * alone. */
    memset(&opts, 0, sizeof opts);
    for (i = 0; i < KL_OPT_COUNT; i++) {
        opts.present |= 1u << i;
        opts.value[i] = h->set[i];
    }
    opts.bank = h->bank[0] ? h->bank : NULL;
    if (compile_arena(h, &opts))
        return no_audio(h, KL_API_NO_MEMORY);
    if (kl_compile(&h->L, &opts, &h->A, &h->C) != KL_COMPILE_OK)
        return no_audio(h, KL_API_FAILED);
    h->compiled = 1;
    if (collect_warnings(h))
        return no_audio(h, KL_API_NO_MEMORY);
    h->sounding = count_sounding(&h->C, &has_pause);

    /* Nothing to hear and nothing to wait for: render no audio at all,
     * rather than the compiler's 250 ms of fade and trail-off, so an index
     * on its own costs no silence. */
    if (h->sounding <= 0 && !has_pause)
        return no_audio(h, 0);

    /* The chunked mix. Every voice is live at once, so each gets its own
     * synth and its own slice of the sample-domain schedule. */
    ne = kl_stream_need_events(&h->C);
    if (GROW(h->S.synth, h->synth_cap, h->C.n_voices) ||
        GROW(h->S.voice_n, h->voice_n_cap, h->C.n_voices) ||
        GROW(h->S.at_sample, h->at_cap, ne ? ne : 1) ||
        GROW(h->S.transition_len, h->trans_cap, ne ? ne : 1))
        return no_audio(h, KL_API_NO_MEMORY);
    h->S.vbuf = h->vbuf;
    h->S.vbuf_cap = KL_API_CHUNK;
    kl_stream_start(&h->S, &h->C, h->sr);
    h->state = ST_RENDERING;
    place_indexes(h);
    return (long)h->S.total;
}

long kl_api_sounding(const kl_api *h)
{
    return (h && h->compiled) ? (h->sounding > 0 ? h->sounding : 0) : 0;
}

const char *kl_api_warnings(const kl_api *h)
{
    return (h && h->warn) ? h->warn : "";
}

/* ------------------------------------------------------------ rendering -- */

void kl_api_cancel(kl_api *h)
{
    if (h) h->cancelled = 1;
}

int kl_api_render_float(kl_api *h, float *out, int cap, int *index)
{
    size_t n, limit;

    if (index) *index = -1;
    if (!h || !out || !index || cap <= 0) return KL_API_BAD_ARG;
    if (h->state != ST_RENDERING) return 0;

    /* An index already due -- at this position, or anywhere once
     * cancelled -- comes out before any more audio, on its own. */
    if (h->idx_next < h->n_idx &&
        (h->cancelled || h->idx[h->idx_next].sample <= h->S.pos)) {
        *index = h->idx[h->idx_next++].id;
        return 0;
    }
    if (h->cancelled) {
        h->state = ST_DONE;
        return 0;
    }

    limit = h->S.total - h->S.pos;
    if (h->idx_next < h->n_idx && h->idx[h->idx_next].sample - h->S.pos < limit)
        limit = h->idx[h->idx_next].sample - h->S.pos;
    n = (size_t)cap < limit ? (size_t)cap : limit;
    if (n == 0) {
        h->state = ST_DONE;
        return 0;
    }

    /* Each voice renders into the handle's vbuf and is summed into `out`,
     * so `out` may be any caller's array, fbuf included. */
    n = kl_stream_render(&h->S, out, n);

    if (h->idx_next < h->n_idx && h->idx[h->idx_next].sample == h->S.pos)
        *index = h->idx[h->idx_next++].id;
    return (int)n;
}

int kl_api_render(kl_api *h, int16_t *out, int cap, int *index)
{
    double g;
    int n, i;

    if (index) *index = -1;
    if (!h || !out || !index || cap <= 0) return KL_API_BAD_ARG;
    if (cap > KL_API_CHUNK) cap = KL_API_CHUNK;
    n = kl_api_render_float(h, h->fbuf, cap, index);
    if (n <= 0) return n;

    g = KL_API_OUTPUT_GAIN * h->set[KL_SET_VOLUME];
    for (i = 0; i < n; i++) {
        double s = (double)h->fbuf[i] * g, q;
        if (s > 1.0) s = 1.0;
        else if (s < -1.0) s = -1.0;
        q = kl_wav_round(s * 32767.0);
        out[i] = (int16_t)(isfinite(q) ? q : 0.0);
    }
    return n;
}
