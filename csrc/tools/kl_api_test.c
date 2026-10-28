/* The exit test of step 5 (docs/21-stage-api.md): the streaming API against
 * the whole-utterance path it replaces.
 *
 *   kl_api_test stream <cases-compile.bin>   the chunked mix, bit for bit
 *   kl_api_test api    <text-corpus.txt>     the API end to end: audio,
 *                                            indexes, cancel, silence
 *   kl_api_test level  <text-corpus.txt>     the generator's normalization
 *                                            gain per line, for §21.4
 *   kl_api_test bench  <text-corpus.txt>     time to first chunk, real-time
 *                                            ratio, for SCREEN-READER.md §8 A
 *
 * `stream` and `api` exit 0 when everything holds and 1 on the first thing
 * that does not, saying what. `level` and `bench` measure and print; they
 * have no pass mark.
 *
 * The reference in every comparison is the pipeline the generator and the
 * stage 6 WAV goldens use -- kl_tokenize, kl_compile, kl_render_mix -- so
 * "equal" here means "equal to what is already proved equal to the
 * JavaScript". Floats are compared by their bits.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies (the engine under test)
 * Copyright (c) 2026 Päiv Dengo (this test)
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "klattsch_api.h"
#include "kl_compile.h"
#include "kl_render.h"
#include "kl_stream.h"
#include "kl_text.h"
#include "kl_token.h"
#include "kl_wav.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; return 1; } } while (0)

/* --- files ---------------------------------------------------------------- */

static unsigned char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    unsigned char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    b = malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    b[sz] = 0;
    *out_len = (size_t)sz;
    return b;
}

static uint32_t rd_u32(const unsigned char *b, size_t p)
{
    return (uint32_t)b[p] | ((uint32_t)b[p+1] << 8)
         | ((uint32_t)b[p+2] << 16) | ((uint32_t)b[p+3] << 24);
}

static double rd_f64(const unsigned char *b, size_t p)
{
    double v;
    memcpy(&v, b + p, 8);
    return v;
}

static char *dup_span(const unsigned char *b, size_t p, uint32_t len)
{
    char *s = malloc((size_t)len + 1);
    if (!s) return NULL;
    if (len) memcpy(s, b + p, len);
    s[len] = '\0';
    return s;
}

/* The text corpus: one input per line, `#` comments and blank lines
 * skipped, as tools/verify-text.mjs reads it. Lines are cut in place. */
static size_t corpus_lines(char *text, char **lines, size_t cap)
{
    size_t n = 0;
    char *p = text;
    while (*p && n < cap) {
        char *e = p;
        while (*e && *e != '\n') e++;
        if (*e) *e++ = '\0';
        {
            size_t l = strlen(p);
            if (l && p[l - 1] == '\r') p[l - 1] = '\0';   /* a CRLF checkout */
        }
        if (*p && *p != '#') lines[n++] = p;
        p = e;
    }
    return n;
}

static int has_alnum(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x80 && isalnum((unsigned char)*s)) return 1;
    return 0;
}

/* --- the reference: tokenize, compile, kl_render_mix ---------------------- */

typedef struct {
    kl_token_list L;
    kl_compile_arena A;
    kl_compiled C;
    float *mix;
    size_t mix_n;
} whole;

static void whole_free(whole *w)
{
    free(w->L.source); free(w->L.tokens); free(w->L.arena);
    free(w->A.voices); free(w->A.events); free(w->A.spans); free(w->A.extras);
    free(w->A.phrases); free(w->A.warnings); free(w->A.text); free(w->A.live);
    free((void *)w->A.syl);
    free(w->mix);
    memset(w, 0, sizeof *w);
}

/* Compile only; returns 0 on success. */
static int whole_compile(whole *w, const char *text, size_t len, const kl_compile_opts *opts)
{
    size_t ns, nt, na;
    memset(w, 0, sizeof *w);
    kl_token_need(len, &ns, &nt, &na);
    w->L.source = malloc(ns * sizeof *w->L.source);
    w->L.tokens = malloc(nt * sizeof *w->L.tokens);
    w->L.arena  = malloc(na ? na : 1);
    w->L.source_cap = ns; w->L.tokens_cap = nt; w->L.arena_cap = na;
    if (!w->L.source || !w->L.tokens || !w->L.arena) return 2;
    if (kl_tokenize(text, len, &w->L) != KL_TOKEN_OK) return 1;

    kl_compile_need(&w->L, opts, &w->A);
    w->A.voices   = calloc(w->A.voices_cap   ? w->A.voices_cap   : 1, sizeof *w->A.voices);
    w->A.events   = calloc(w->A.events_cap   ? w->A.events_cap   : 1, sizeof *w->A.events);
    w->A.spans    = calloc(w->A.events_cap   ? w->A.events_cap   : 1, sizeof *w->A.spans);
    w->A.extras   = calloc(w->A.extras_cap   ? w->A.extras_cap   : 1, sizeof *w->A.extras);
    w->A.phrases  = calloc(w->A.phrases_cap  ? w->A.phrases_cap  : 1, sizeof *w->A.phrases);
    w->A.warnings = calloc(w->A.warnings_cap ? w->A.warnings_cap : 1, sizeof *w->A.warnings);
    w->A.text     = calloc(w->A.text_cap     ? w->A.text_cap     : 1, 1);
    w->A.live     = calloc(w->A.live_cap     ? w->A.live_cap     : 1, sizeof *w->A.live);
    w->A.syl      = calloc(w->A.syl_cap      ? w->A.syl_cap      : 1, sizeof *w->A.syl);
    if (!w->A.voices || !w->A.events || !w->A.spans || !w->A.extras || !w->A.phrases
        || !w->A.warnings || !w->A.text || !w->A.live || !w->A.syl) return 2;
    return kl_compile(&w->L, opts, &w->A, &w->C) == KL_COMPILE_OK ? 0 : 1;
}

/* Compile and render whole. */
static int whole_render(whole *w, const char *text, size_t len,
                        const kl_compile_opts *opts, double rate)
{
    kl_render_arena R;
    size_t sched;
    int rc = whole_compile(w, text, len, opts);
    if (rc) return rc;
    w->mix_n = kl_render_samples_for(w->C.total_ms, rate);
    sched = kl_render_need_sched(&w->C);
    memset(&R, 0, sizeof R);
    w->mix = calloc(w->mix_n ? w->mix_n : 1, sizeof *w->mix);
    R.scratch = calloc(w->mix_n ? w->mix_n : 1, sizeof *R.scratch);
    R.scratch_cap = w->mix_n;
    R.at_sample = calloc(sched ? sched : 1, sizeof *R.at_sample);
    R.transition_len = calloc(sched ? sched : 1, sizeof *R.transition_len);
    R.sched_cap = sched;
    rc = (!w->mix || !R.scratch || !R.at_sample || !R.transition_len) ? 2
       : kl_render_mix(&w->C, rate, w->mix, w->mix_n, &R) == KL_RENDER_OK ? 0 : 1;
    free(R.scratch); free(R.at_sample); free(R.transition_len);
    return rc;
}

/* The options the API compiles with: all ten present, at the defaults. */
static kl_compile_opts default_opts(void)
{
    static const double v[KL_OPT_COUNT] = {
        KL_DEFAULT_BASE_F0, KL_DEFAULT_RATE, 1.0, 0.0, 5.0, 0.0, 5.0, 0.0, 0.0, 0.5 };
    kl_compile_opts o;
    int i;
    memset(&o, 0, sizeof o);
    for (i = 0; i < KL_OPT_COUNT; i++) {
        o.present |= 1u << i;
        o.value[i] = v[i];
    }
    return o;
}

/* --- stream: kl_stream against kl_render_mix, the whole corpus ----------- */

static int stream_one(const whole *w, double rate, size_t chunk, uint32_t c)
{
    kl_stream S;
    size_t ne = kl_stream_need_events(&w->C), got = 0, n;
    float *out = malloc((w->mix_n ? w->mix_n : 1) * sizeof *out);
    int ok;

    memset(&S, 0, sizeof S);
    S.synth = calloc(w->C.n_voices ? w->C.n_voices : 1, sizeof *S.synth);
    S.voice_n = calloc(w->C.n_voices ? w->C.n_voices : 1, sizeof *S.voice_n);
    S.at_sample = calloc(ne ? ne : 1, sizeof *S.at_sample);
    S.transition_len = calloc(ne ? ne : 1, sizeof *S.transition_len);
    S.vbuf = calloc(chunk, sizeof *S.vbuf);
    S.vbuf_cap = chunk;
    if (!out || !S.synth || !S.voice_n || !S.at_sample || !S.transition_len || !S.vbuf) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    kl_stream_start(&S, &w->C, rate);
    while ((n = kl_stream_render(&S, out + got, chunk)) > 0) got += n;

    ok = got == w->mix_n && (got == 0 || memcmp(out, w->mix, got * sizeof *out) == 0);
    if (!ok) {
        size_t i = 0;
        while (i < got && i < w->mix_n && memcmp(&out[i], &w->mix[i], sizeof *out) == 0) i++;
        fprintf(stderr, "FAIL stream: case %u, %.0f Hz, chunk %zu: %zu samples vs %zu, "
                "first difference at %zu\n", c, rate, chunk, got, w->mix_n, i);
        failures++;
    }
    free(out); free(S.synth); free(S.voice_n); free(S.at_sample);
    free(S.transition_len); free(S.vbuf);
    return ok ? 0 : 1;
}

static int run_stream(const char *path)
{
    /* Three rates, as the stage 6 goldens; every chunk size at the rate in the
     * middle, and the uneven one at the other two. 1 is the size that most
     * often lands a boundary on a schedule event. */
    static const struct { double rate; size_t chunk; } RUNS[] = {
        { 22050, 1 }, { 22050, 137 }, { 22050, 4096 },
        { 48000, 137 }, { 8000, 137 },
    };
    size_t blen, p = 4;
    unsigned char *blob = slurp(path, &blen);
    uint32_t n_cases, c;
    size_t r, compared = 0, samples = 0;

    if (!blob) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    n_cases = rd_u32(blob, 0);

    for (c = 0; c < n_cases; c++) {
        uint32_t text_len, bank_len, engine_len, n_extra, i;
        const char *text;
        char *bank = NULL, *engine = NULL, **keys = NULL;
        kl_opt_extra *extras = NULL;
        kl_compile_opts opts;

        text_len = rd_u32(blob, p); p += 4;
        text = (const char *)blob + p; p += text_len;
        memset(&opts, 0, sizeof opts);
        opts.present = rd_u32(blob, p); p += 4;
        for (i = 0; i < (uint32_t)KL_OPT_COUNT; i++) { opts.value[i] = rd_f64(blob, p); p += 8; }
        bank_len = rd_u32(blob, p); p += 4;
        if (bank_len) { bank = dup_span(blob, p, bank_len); opts.bank = bank; }
        p += bank_len;
        engine_len = rd_u32(blob, p); p += 4;
        if (engine_len) { engine = dup_span(blob, p, engine_len); opts.engine = engine; }
        p += engine_len;
        n_extra = rd_u32(blob, p); p += 4;
        if (n_extra) {
            extras = calloc(n_extra, sizeof *extras);
            keys = calloc(n_extra, sizeof *keys);
        }
        for (i = 0; i < n_extra; i++) {
            uint32_t kl = rd_u32(blob, p); p += 4;
            keys[i] = dup_span(blob, p, kl); p += kl;
            extras[i].key = keys[i];
            extras[i].value = rd_f64(blob, p); p += 8;
        }
        opts.extras = extras;
        opts.n_extras = n_extra;

        for (r = 0; r < sizeof RUNS / sizeof RUNS[0]; r++) {
            whole w;
            int rc = whole_render(&w, text, text_len, &opts, RUNS[r].rate);
            if (rc) {
                fprintf(stderr, "case %u: reference path failed (%d)\n", c, rc);
                return 2;
            }
            stream_one(&w, RUNS[r].rate, RUNS[r].chunk, c);
            compared++;
            samples += w.mix_n;
            whole_free(&w);
        }

        for (i = 0; i < n_extra; i++) free(keys[i]);
        free(keys); free(extras); free(bank); free(engine);
    }
    free(blob);
    printf("stream: %u cases, %zu renders, %zu samples, %d differing\n",
           n_cases, compared, samples, failures);
    return failures ? 1 : 0;
}

/* --- api: the API end to end --------------------------------------------- */

#define RATE 22050

typedef struct {
    float *f;      size_t n, cap;
    int   *ids;    size_t *at; size_t n_ids, ids_cap;
} drained;

static void drained_free(drained *d)
{
    free(d->f); free(d->ids); free(d->at);
    memset(d, 0, sizeof *d);
}

/* Render to the end in chunks of `chunk`, recording every index and the
 * sample position it came out at. Stops early after `stop_after` calls that
 * returned audio, cancelling there, when stop_after >= 0. */
static void drain(kl_api *h, int chunk, long stop_after, drained *d)
{
    float buf[4096];
    long audio_calls = 0;
    int n, index;
    memset(d, 0, sizeof *d);
    for (;;) {
        if (stop_after >= 0 && audio_calls == stop_after) {
            kl_api_cancel(h);
            stop_after = -1;
        }
        n = kl_api_render_float(h, buf, chunk, &index);
        if (n < 0) { fprintf(stderr, "render error %d\n", n); exit(2); }
        if (n > 0) {
            if (d->n + (size_t)n > d->cap) {
                d->cap = (d->n + (size_t)n) * 2;
                d->f = realloc(d->f, d->cap * sizeof *d->f);
            }
            memcpy(d->f + d->n, buf, (size_t)n * sizeof *buf);
            d->n += (size_t)n;
            audio_calls++;
        }
        if (index >= 0 || index < -1) {
            if (d->n_ids == d->ids_cap) {
                d->ids_cap = d->ids_cap ? d->ids_cap * 2 : 16;
                d->ids = realloc(d->ids, d->ids_cap * sizeof *d->ids);
                d->at = realloc(d->at, d->ids_cap * sizeof *d->at);
            }
            d->ids[d->n_ids] = index;
            d->at[d->n_ids++] = d->n;
        }
        if (n == 0 && index == -1) break;
    }
}

static kl_text_opts api_text_opts(void)
{
    kl_text_opts o;
    memset(&o, 0, sizeof o);
    o.base_f0 = KL_DEFAULT_BASE_F0;
    o.inflection = 1.0;
    o.set_inflection = 1;
    return o;
}

static char *to_source(kl_text_ctx *x, const char *text, const kl_text_opts *o)
{
    size_t need = kl_text_to_source(x, text, o, NULL, 0);
    char *s = malloc(need + 1);
    kl_text_to_source(x, text, o, s, need + 1);
    return s;
}

/* Every corpus line: the front end with inflection 1 writes what it wrote
 * without the option; the API's stream equals the reference for that
 * source; text with letters or digits makes a sound. */
static int api_lines(kl_api *h, kl_text_ctx *x, char **lines, size_t n_lines)
{
    kl_compile_opts opts = default_opts();
    kl_text_opts to = api_text_opts();
    size_t i;
    for (i = 0; i < n_lines; i++) {
        char *a = to_source(x, lines[i], NULL);
        char *b = to_source(x, lines[i], &to);
        whole w;
        drained d;
        long total;

        CHECK(strcmp(a, b) == 0, "line %zu: inflection 1 changed the source\n  %s\n  %s", i, a, b);

        kl_api_begin(h);
        CHECK(kl_api_text(h, lines[i]) == KL_API_OK, "line %zu: kl_api_text", i);
        total = kl_api_end(h);
        CHECK(total >= 0, "line %zu: kl_api_end %ld", i, total);
        if (has_alnum(lines[i]))
            CHECK(kl_api_sounding(h) > 0, "line %zu is silent: \"%s\" -> \"%s\"", i, lines[i], a);
        drain(h, 1000, -1, &d);

        if (total > 0) {
            CHECK(whole_render(&w, a, strlen(a), &opts, RATE) == 0, "line %zu: reference", i);
            CHECK(d.n == w.mix_n && (size_t)total == w.mix_n &&
                  memcmp(d.f, w.mix, d.n * sizeof *d.f) == 0,
                  "line %zu: stream differs from the reference (%zu vs %zu samples)", i, d.n, w.mix_n);
            whole_free(&w);
        } else {
            CHECK(d.n == 0, "line %zu: audio from an empty utterance", i);
        }
        drained_free(&d);
        free(a);
        free(b);
    }
    printf("api: %zu corpus lines, inflection 1 unchanged, streams equal, none silent\n", n_lines);
    return 0;
}

/* Indexes on their own: no audio, every index once, in order. */
static int api_index_only(kl_api *h)
{
    drained d;
    kl_api_begin(h);
    kl_api_index(h, 7);
    kl_api_text(h, "");
    kl_api_index(h, 8);
    kl_api_text(h, "   ");
    kl_api_index(h, 9);
    CHECK(kl_api_end(h) == 0, "index-only utterance has audio");
    drain(h, 512, -1, &d);
    CHECK(d.n == 0 && d.n_ids == 3 && d.ids[0] == 7 && d.ids[1] == 8 && d.ids[2] == 9,
          "index-only: %zu samples, %zu indexes", d.n, d.n_ids);
    drained_free(&d);

    kl_api_begin(h);
    CHECK(kl_api_end(h) == 0, "empty utterance has audio");
    drain(h, 512, -1, &d);
    CHECK(d.n == 0 && d.n_ids == 0, "empty utterance yielded something");
    drained_free(&d);

    /* Unknown phonemes only: nothing to hear, no pause, so no audio. */
    kl_api_begin(h);
    kl_api_source(h, "XX QQ");
    kl_api_index(h, 1);
    CHECK(kl_api_end(h) == 0 && kl_api_sounding(h) == 0, "unknown phonemes made audio");
    drain(h, 512, -1, &d);
    CHECK(d.n == 0 && d.n_ids == 1 && d.ids[0] == 1, "unknown phonemes: index lost");
    drained_free(&d);

    /* A break alone is a pause to wait for, and renders. */
    kl_api_begin(h);
    kl_api_break(h, 500);
    kl_api_index(h, 1);
    CHECK(kl_api_end(h) > 0 && kl_api_sounding(h) == 0, "a break rendered nothing");
    drain(h, 512, -1, &d);
    CHECK(d.n_ids == 1 && d.at[0] == d.n, "break: index not at the end");
    drained_free(&d);
    printf("api: index-only, empty, unknown-phoneme and break-only utterances\n");
    return 0;
}

/* Where indexes fall, worked out by hand. Unstressed phonemes at the default
 * rate take 110 ms each, so the fourth ends at 440 ms: sample
 * ceil(440 * 22050 / 1000) = 9702. An index after the last piece fires at
 * the end of the audio, an index before the first at 0. A homoglyph
 * (Cyrillic O, two bytes, one unit after normalization) must not move them. */
static int api_index_places(kl_api *h)
{
    static const char *const FIRST[] = { "HH AH L OW", "HH AH L \xD0\x9EW" };
    size_t k;
    for (k = 0; k < 2; k++) {
        drained d;
        long total;
        kl_api_begin(h);
        kl_api_index(h, 0);
        kl_api_source(h, FIRST[k]);
        kl_api_index(h, 1);
        kl_api_source(h, "W ER L D");
        kl_api_index(h, 2);
        total = kl_api_end(h);
        CHECK(total > 0, "places: no audio");
        drain(h, 300, -1, &d);
        CHECK(d.n_ids == 3 && d.ids[0] == 0 && d.ids[1] == 1 && d.ids[2] == 2,
              "places (%s): %zu indexes", k ? "homoglyph" : "ascii", d.n_ids);
        CHECK(d.at[0] == 0, "places: index 0 at %zu", d.at[0]);
        CHECK(d.at[1] == 9702, "places (%s): index 1 at %zu, not 9702",
              k ? "homoglyph" : "ascii", d.at[1]);
        CHECK(d.at[2] == (size_t)total && d.n == (size_t)total,
              "places: index 2 at %zu, total %ld", d.at[2], total);
        drained_free(&d);
    }
    /* A place that is not a whole sample: one phoneme is 110 ms, which is
     * 2425.5 samples, and the index fires at the first whole sample at or
     * after it -- ceil, as every sample count in this port is. */
    {
        drained d;
        kl_api_begin(h);
        kl_api_source(h, "AH");
        kl_api_index(h, 1);
        kl_api_source(h, "AH");
        kl_api_end(h);
        drain(h, 300, -1, &d);
        CHECK(d.n_ids == 1 && d.at[0] == 2426, "index at 110 ms came out at %zu, not 2426",
              d.n_ids ? d.at[0] : 0);
        drained_free(&d);
    }
    printf("api: index places -- 0, 9702 and 2426 (by hand), the end; unmoved by a homoglyph\n");
    return 0;
}

/* Pieces cannot reach into each other: a `#` comment ends with its piece,
 * and a break of 0 is no break at all -- `p0` would dip the next 30 ms. */
static int api_joins(kl_api *h)
{
    drained a, b;
    kl_api_begin(h);
    kl_api_source(h, "HH AH # a comment to the end of the piece");
    kl_api_source(h, "L OW");
    kl_api_end(h);
    CHECK(kl_api_sounding(h) == 4, "a comment swallowed the next piece: %ld sounding",
          kl_api_sounding(h));

    kl_api_begin(h);
    kl_api_source(h, "HH AH");
    kl_api_source(h, "L OW");
    kl_api_end(h);
    drain(h, 4096, -1, &a);
    kl_api_begin(h);
    kl_api_source(h, "HH AH");
    kl_api_break(h, 0);
    kl_api_source(h, "L OW");
    kl_api_end(h);
    drain(h, 4096, -1, &b);
    CHECK(a.n == b.n && memcmp(a.f, b.f, a.n * sizeof *a.f) == 0, "a break of 0 changed the audio");
    drained_free(&a);
    drained_free(&b);
    printf("api: a comment ends with its piece; a break of 0 changes nothing\n");
    return 0;
}

/* Inflection reaches the front end: 0 writes no pitch movement at all, 2
 * doubles the start level ("Hello, world." at 120 Hz: +9.6 Hz, so +19.2). */
static int api_inflection(kl_text_ctx *x)
{
    kl_text_opts o = api_text_opts();
    char *s;
    o.inflection = 0.0;
    s = to_source(x, "Hello, world. Is it?", &o);
    CHECK(strchr(s, '+') == NULL && strchr(s, '-') == NULL,
          "inflection 0 still moves pitch: %s", s);
    free(s);
    o.inflection = 2.0;
    s = to_source(x, "Hello, world.", &o);
    CHECK(strstr(s, "b+19.2 ") != NULL, "inflection 2 did not double the start: %s", s);
    free(s);
    printf("api: inflection 0 is flat, 2 is twice the contour\n");
    return 0;
}

/* Several pieces with indexes between them: the stream equals the reference
 * for the joined source, at several chunk sizes, and the indexes come out in
 * order, once each, at non-decreasing places. Then a cancel at every chunk
 * boundary: no audio after it, and every index still comes out once. */
static int api_pieces(kl_api *h, char **lines, size_t n_lines)
{
    static const int CHUNKS[] = { 1, 441, 4096 };
    const size_t N = n_lines < 6 ? n_lines : 6;
    kl_compile_opts opts = default_opts();
    whole w = { 0 };
    size_t i, c;
    long total = 0, chunks, k;

    for (c = 0; c < sizeof CHUNKS / sizeof CHUNKS[0]; c++) {
        drained d;
        kl_api_begin(h);
        for (i = 0; i < N; i++) {
            kl_api_text(h, lines[i]);
            kl_api_index(h, (int)i);
        }
        total = kl_api_end(h);
        CHECK(total > 0, "pieces: no audio");
        if (c == 0) {
            /* The reference: the API's own source, compiled and mixed whole.
             * kl_api has no getter for it, so it is rebuilt the same way. */
            kl_text_ctx *x = calloc(1, sizeof *x);
            kl_text_opts to = api_text_opts();
            size_t len = 0;
            char *joined = malloc(1);
            joined[0] = '\0';
            for (i = 0; i < N; i++) {
                char *s = to_source(x, lines[i], &to);
                size_t sl = strlen(s);
                if (!sl) { free(s); continue; }
                joined = realloc(joined, len + sl + 2);
                if (len) joined[len++] = '\n';
                memcpy(joined + len, s, sl + 1);
                len += sl;
                free(s);
            }
            CHECK(whole_render(&w, joined, len, &opts, RATE) == 0, "pieces: reference");
            free(joined);
            free(x);
        }
        drain(h, CHUNKS[c], -1, &d);
        CHECK(d.n == w.mix_n && memcmp(d.f, w.mix, d.n * sizeof *d.f) == 0,
              "pieces, chunk %d: stream differs from the joined reference", CHUNKS[c]);
        CHECK(d.n_ids == N, "pieces, chunk %d: %zu indexes of %zu", CHUNKS[c], d.n_ids, N);
        for (i = 0; i < N; i++) {
            CHECK(d.ids[i] == (int)i, "pieces: index %zu is %d", i, d.ids[i]);
            CHECK(i == 0 || d.at[i] >= d.at[i - 1], "pieces: index %zu goes backwards", i);
        }
        CHECK(d.at[N - 1] == d.n, "pieces: last index not at the end");
        drained_free(&d);
    }
    whole_free(&w);

    /* Cancel at every boundary of 2048-sample chunks. */
    chunks = (total + 2047) / 2048;
    for (k = 0; k <= chunks; k++) {
        drained d;
        kl_api_begin(h);
        for (i = 0; i < N; i++) {
            kl_api_text(h, lines[i]);
            kl_api_index(h, (int)i);
        }
        kl_api_end(h);
        drain(h, 2048, k, &d);
        CHECK(d.n <= (size_t)k * 2048, "cancel after %ld chunks: %zu samples came out", k, d.n);
        CHECK(d.n_ids == N, "cancel after %ld chunks: %zu indexes of %zu", k, d.n_ids, N);
        for (i = 0; i < N; i++)
            CHECK(d.ids[i] == (int)i, "cancel after %ld: index %zu is %d", k, i, d.ids[i]);
        drained_free(&d);
    }
    printf("api: %zu pieces -- streams equal at chunks 1, 441, 4096; "
           "cancel at each of %ld boundaries keeps every index\n", N, chunks + 1);
    return 0;
}

/* The 16-bit path is the float path times the gain and volume, clamped and
 * rounded the way kl_wav rounds. */
static int api_level(kl_api *h, const char *line)
{
    static const double VOLUMES[] = { 1.0, 0.5, 0.0 };
    size_t v;
    for (v = 0; v < 3; v++) {
        float f[1024];
        int16_t q[1024];
        int n, m, i, idx;
        double g = kl_api_output_gain() * VOLUMES[v];
        kl_api_set(h, KL_SET_VOLUME, VOLUMES[v]);
        kl_api_begin(h); kl_api_text(h, line); kl_api_end(h);
        n = kl_api_render_float(h, f, 1024, &idx);
        kl_api_begin(h); kl_api_text(h, line); kl_api_end(h);
        m = kl_api_render(h, q, 1024, &idx);
        CHECK(n == m && n > 0, "level: %d float vs %d int16 samples", n, m);
        for (i = 0; i < n; i++) {
            double s = (double)f[i] * g;
            if (s > 1.0) s = 1.0;
            if (s < -1.0) s = -1.0;
            CHECK(q[i] == (int16_t)kl_wav_round(s * 32767.0),
                  "level %.1f: sample %d is %d, not %d", VOLUMES[v], i, q[i],
                  (int)kl_wav_round(s * 32767.0));
        }
    }
    kl_api_set(h, KL_SET_VOLUME, 1.0);
    printf("api: 16-bit level at volume 1, 0.5, 0\n");
    return 0;
}

static int api_settings(kl_api *h)
{
    CHECK(kl_api_set(h, KL_SET_RATE, 0.0) == KL_API_BAD_ARG, "rate 0 accepted");
    CHECK(kl_api_set(h, KL_SET_RATE, NAN) == KL_API_BAD_ARG, "NaN accepted");
    CHECK(kl_api_set(h, KL_SET_VOLUME, -1.0) == KL_API_BAD_ARG, "negative volume accepted");
    CHECK(kl_api_set(h, KL_SET_COUNT, 1.0) == KL_API_BAD_ARG, "unknown setting accepted");
    CHECK(kl_api_set_bank(h, "no-such-bank") == KL_API_BAD_BANK, "unknown bank accepted");
    CHECK(kl_api_get(h, KL_SET_RATE) == KL_DEFAULT_RATE, "a refused setting changed the value");
    CHECK(kl_api_render(h, NULL, 10, NULL) == KL_API_BAD_ARG, "NULL render accepted");
    printf("api: settings refuse what they should\n");
    return 0;
}

static int run_api(const char *path)
{
    size_t len, n;
    unsigned char *text = slurp(path, &len);
    char *lines[512];
    kl_api *h;
    kl_text_ctx *x;

    if (!text) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    n = corpus_lines((char *)text, lines, 512);
    h = kl_api_create(RATE);
    x = calloc(1, sizeof *x);
    if (!h || !x) { fprintf(stderr, "out of memory\n"); return 2; }

    if (!api_settings(h) && !api_lines(h, x, lines, n) && !api_inflection(x) &&
        !api_index_only(h) && !api_index_places(h) && !api_joins(h) &&
        !api_pieces(h, lines, n))
        api_level(h, lines[0]);

    kl_api_destroy(h);
    free(x);
    free(text);
    return failures ? 1 : 0;
}

/* --- level: what the generator's normalization does to each line -------- */

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int run_level(const char *path)
{
    size_t len, n, i, k = 0;
    unsigned char *text = slurp(path, &len);
    char *lines[512];
    double gains[512], peaks[512];
    kl_api *h;

    if (!text) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    n = corpus_lines((char *)text, lines, 512);
    h = kl_api_create(RATE);
    for (i = 0; i < n; i++) {
        drained d;
        double peak = 0.0;
        size_t j;
        kl_api_begin(h);
        kl_api_text(h, lines[i]);
        if (kl_api_end(h) <= 0) continue;
        drain(h, 4096, -1, &d);
        for (j = 0; j < d.n; j++) {
            double a = fabs((double)d.f[j]);
            if (a > peak) peak = a;
        }
        drained_free(&d);
        if (peak <= 0.0) continue;
        peaks[k] = peak;
        gains[k++] = KL_WAV_PEAK_NORMALIZE / peak;
    }
    qsort(gains, k, sizeof *gains, cmp_double);
    qsort(peaks, k, sizeof *peaks, cmp_double);
    printf("level: %zu lines at %d Hz\n", k, RATE);
    printf("  peak of the float mix: min %.4f  p10 %.4f  median %.4f  p90 %.4f  max %.4f\n",
           peaks[0], peaks[k / 10], peaks[k / 2], peaks[k * 9 / 10], peaks[k - 1]);
    printf("  generator's gain (%.2f / peak): min %.4f  p10 %.4f  median %.4f  p90 %.4f  max %.4f\n",
           KL_WAV_PEAK_NORMALIZE, gains[0], gains[k / 10], gains[k / 2], gains[k * 9 / 10], gains[k - 1]);
    kl_api_destroy(h);
    free(text);
    return 0;
}

/* --- bench: time to first chunk, real-time ratio ------------------------- */

static double now_s(void)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int run_bench(const char *path)
{
    static const int RATES[] = { 22050, 48000 };
    size_t len, n, i, r;
    unsigned char *text = slurp(path, &len);
    char *lines[512];

    if (!text) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    n = corpus_lines((char *)text, lines, 512);

    for (r = 0; r < 2; r++) {
        kl_api *h = kl_api_create(RATES[r]);
        int16_t buf[4096];
        int chunk = RATES[r] / 50;     /* 20 ms */
        int idx, reps, rep;
        double first = 1e9, t0, t1, audio = 0.0, spent = 0.0;
        long total = 0;

        /* Warm the buffers once, so growth is not what is measured. */
        kl_api_begin(h);
        for (i = 0; i < n; i++) kl_api_text(h, lines[i]);
        kl_api_end(h);
        while (kl_api_render(h, buf, 4096, &idx) > 0 || idx >= 0) {}

        reps = 5;
        for (rep = 0; rep < reps; rep++) {
            /* The whole corpus as one paragraph: time to its first 20 ms. */
            t0 = now_s();
            kl_api_begin(h);
            for (i = 0; i < n; i++) kl_api_text(h, lines[i]);
            total = kl_api_end(h);
            kl_api_render(h, buf, chunk, &idx);
            t1 = now_s();
            if (t1 - t0 < first) first = t1 - t0;
            /* ... and the rest of it, for the ratio. */
            while (kl_api_render(h, buf, chunk, &idx) > 0 || idx >= 0) {}
            spent += now_s() - t0;
            audio += (double)total / RATES[r];
        }
        printf("bench %d Hz: %zu lines, %.1f s of audio per run; first 20 ms after %.2f ms "
               "(best of %d); %.0fx real time\n",
               RATES[r], n, audio / reps, first * 1000.0, reps, audio / spent);
        kl_api_destroy(h);
    }
    free(text);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "stream") == 0) return run_stream(argv[2]);
    if (argc == 3 && strcmp(argv[1], "api") == 0)    return run_api(argv[2]);
    if (argc == 3 && strcmp(argv[1], "level") == 0)  return run_level(argv[2]);
    if (argc == 3 && strcmp(argv[1], "bench") == 0)  return run_bench(argv[2]);
    fprintf(stderr, "usage: kl_api_test stream|api|level|bench <file>\n");
    return 2;
}
