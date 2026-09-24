/* Stage 6 verification: render every corpus case the way bin/klattsch.mjs
 * does and emit the resulting WAV files for tools/verify-stage6.mjs.
 *
 *   kl_wav_dump <cases-compile.bin> <sample-rate>
 *   kl_wav_dump --round-sweep
 *
 * The blob is goldens/cases-compile.bin, the same file stage 5 reads: each
 * case is its text plus the opts compileSection needs, because 262 of the 729
 * cases carry a bank, an engine or seeded extras and a renderer driven only
 * by text would never reach those paths.
 *
 * Whole files go out, not digests. The verifier holds the JavaScript's bytes
 * beside them, so a mismatch is reported as "case X, sample 1403, -1234 vs
 * -1235" rather than as two hex strings that differ.
 *
 * The CLI is not what renders here, and that is deliberate: bin/klattsch.mjs
 * takes a phoneme string and nothing else, so it cannot reach a case that
 * carries a bank. The CLI is instead proved to *be* this pipeline by the
 * end-to-end comparisons in tools/verify-stage6.mjs, which run both real
 * programs and compare the files they write.
 *
 * `--round-sweep` emits (x, kl_wav_round(x)) pairs instead, so that the one
 * place this port cannot use a C library function is swept against the
 * reference rather than asserted to be right. `--wav-goldens` emits the six
 * encodings of goldens/wav.json, which reach the parts of the format the
 * corpus never does: no metadata, normalization off, the clamp, an
 * odd-length comment and a zero-length file.
 *
 * Binary on stdout, little endian.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "kl_compile.h"
#include "kl_fmt.h"
#include "kl_render.h"
#include "kl_token.h"
#include "kl_wav.h"

static void put_u32(uint32_t v)
{
    unsigned char b[4] = { (unsigned char)(v & 0xFFu), (unsigned char)((v >> 8) & 0xFFu),
                           (unsigned char)((v >> 16) & 0xFFu), (unsigned char)((v >> 24) & 0xFFu) };
    fwrite(b, 1, 4, stdout);
}

static void put_f64(double v)
{
    unsigned char b[8];
    memcpy(b, &v, 8);            /* the host is little-endian everywhere this builds */
    fwrite(b, 1, 8, stdout);
}

static unsigned char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    unsigned char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 4) { fclose(f); return NULL; }
    b = malloc((size_t)sz);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
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

/* --- the rounding sweep --------------------------------------------------- */

/* Values chosen for the three ways Math.round and C differ: exact ties at
 * every sign, the 0.49999999999999994 case that defeats floor(x + 0.5), and
 * the neighbours of a tie on both sides. The scaled-sample values at the end
 * are the domain the encoder actually works in. */
static void round_sweep(void)
{
    double vals[512];
    size_t n = 0;
    int k;

    for (k = -12; k <= 12; k++) {
        double half = (double)k + 0.5;
        vals[n++] = (double)k;
        vals[n++] = half;
        vals[n++] = nextafter(half, -1.0e300);
        vals[n++] = nextafter(half, 1.0e300);
        vals[n++] = (double)k + 0.25;
        vals[n++] = (double)k - 0.25;
    }
    vals[n++] = 0.49999999999999994;
    vals[n++] = -0.49999999999999994;
    vals[n++] = 0.0;
    vals[n++] = -0.0;
    vals[n++] = nextafter(0.0, 1.0);
    vals[n++] = nextafter(0.0, -1.0);
    vals[n++] = 32766.5;
    vals[n++] = -32766.5;
    vals[n++] = 32767.0;
    vals[n++] = -32767.0;
    vals[n++] = 0.95 * 32767.0;
    vals[n++] = -0.95 * 32767.0;
    vals[n++] = 4503599627370496.0;        /* 2^52: the last double with a fraction */
    vals[n++] = -4503599627370496.0;
    vals[n++] = 4503599627370495.5;
    vals[n++] = -4503599627370495.5;

    put_u32((uint32_t)n);
    for (k = 0; (size_t)k < n; k++) {
        put_f64(vals[k]);
        put_f64(kl_wav_round(vals[k]));
    }
}

/* --- the toFixed sweep ----------------------------------------------------- */

static void put_str(const char *s)
{
    size_t n = strlen(s);
    put_u32((uint32_t)n);
    fwrite(s, 1, n, stdout);
}

/* kl_to_fixed against Number.prototype.toFixed. The values are chosen to land
 * on the tie set at both digit counts -- k/2 for toFixed(0) and k/8 for
 * toFixed(2) -- plus the byte counts and durations the CLI actually divides,
 * and the decimal literals that are famous for not being what they look like. */
static void tofixed_sweep(void)
{
    static const double odds[] = {
        0.615, 1.005, 8.835, 2.675, 1.1163466119446286, 0.9499999999999999,
        1.0e-7, 0.0, 1.0, 99.995, 12345.6789,
    };
    uint32_t n = 0;
    int k;
    char buf[64];

    n += 61;                 /* k/2,   digits 0 */
    n += 25;                 /* n/1024, digits 0 */
    n += 201;                /* k/8,   digits 2 */
    n += (uint32_t)(sizeof odds / sizeof odds[0]) * 2;
    put_u32(n);

    for (k = 0; k <= 60; k++) {
        double x = (double)k / 2.0;
        kl_to_fixed(buf, sizeof buf, x, 0);
        put_f64(x); put_u32(0); put_str(buf);
    }
    for (k = 0; k < 25; k++) {
        double x = (double)(k * 128) / 1024.0;      /* 512 and 1536 are ties */
        kl_to_fixed(buf, sizeof buf, x, 0);
        put_f64(x); put_u32(0); put_str(buf);
    }
    for (k = 0; k <= 200; k++) {
        double x = (double)k / 8.0;
        kl_to_fixed(buf, sizeof buf, x, 2);
        put_f64(x); put_u32(2); put_str(buf);
    }
    for (k = 0; (size_t)k < sizeof odds / sizeof odds[0]; k++) {
        kl_to_fixed(buf, sizeof buf, odds[k], 0);
        put_f64(odds[k]); put_u32(0); put_str(buf);
        kl_to_fixed(buf, sizeof buf, odds[k], 2);
        put_f64(odds[k]); put_u32(2); put_str(buf);
    }
}

/* --- the encoder goldens --------------------------------------------------- */

/* goldens/wav.json, captured by captureWav() in tools/goldens.mjs: the six
 * encodings of one buffer that pin the parts of the format the corpus does
 * not reach -- no metadata at all, both fields, normalization off, an
 * odd-length comment and its pad byte, the clamp, and a zero-length file.
 *
 * The buffer is "HH AH L OW" at 22050 Hz. The JavaScript renders it with
 * renderToBuffer on voice 0; this renders it with the mix, which is the same
 * thing for a single-voice utterance and is the code the CLI uses. */
static void emit_wav(const float *buf, size_t n, double peak_normalize,
                     const kl_wav_meta *meta)
{
    unsigned char *wav;
    size_t cap = kl_wav_size(n, meta), len;
    double gain = 1.0;

    wav = malloc(cap ? cap : 1);
    if (!wav) { fprintf(stderr, "out of memory\n"); exit(2); }
    len = kl_wav_encode(buf, n, 22050, peak_normalize, meta, wav, cap, &gain);
    if (len != cap) { fprintf(stderr, "encode wrote %zu of %zu\n", len, cap); exit(1); }

    put_u32((uint32_t)len);
    put_f64(gain);
    fwrite(wav, 1, len, stdout);
    free(wav);
}

static int wav_goldens(void)
{
    static const char text[] = "HH AH L OW";
    size_t need_src, need_tok, need_arena;
    kl_token_list L;
    kl_compile_arena A;
    kl_compiled out;
    kl_render_arena R;
    kl_wav_meta meta;
    float *mix, *loud;
    size_t mix_n, sched_cap, i;

    kl_token_need(sizeof text - 1, &need_src, &need_tok, &need_arena);
    memset(&L, 0, sizeof L);
    L.source = malloc(need_src * sizeof(uint16_t));
    L.tokens = malloc(need_tok * sizeof(kl_token));
    L.arena  = malloc(need_arena);
    if (!L.source || !L.tokens || !L.arena) { fprintf(stderr, "out of memory\n"); return 2; }
    L.source_cap = need_src; L.tokens_cap = need_tok; L.arena_cap = need_arena;
    if (kl_tokenize(text, sizeof text - 1, &L) != KL_TOKEN_OK) return 1;

    kl_compile_need(&L, NULL, &A);
    A.voices   = calloc(A.voices_cap   ? A.voices_cap   : 1, sizeof *A.voices);
    A.events   = calloc(A.events_cap   ? A.events_cap   : 1, sizeof *A.events);
    A.spans    = calloc(A.events_cap   ? A.events_cap   : 1, sizeof *A.spans);
    A.extras   = calloc(A.extras_cap   ? A.extras_cap   : 1, sizeof *A.extras);
    A.phrases  = calloc(A.phrases_cap  ? A.phrases_cap  : 1, sizeof *A.phrases);
    A.warnings = calloc(A.warnings_cap ? A.warnings_cap : 1, sizeof *A.warnings);
    A.text     = calloc(A.text_cap     ? A.text_cap     : 1, 1);
    A.live     = calloc(A.live_cap     ? A.live_cap     : 1, sizeof *A.live);
    A.syl      = calloc(A.syl_cap      ? A.syl_cap      : 1, sizeof *A.syl);
    if (!A.voices || !A.events || !A.spans || !A.extras || !A.phrases
        || !A.warnings || !A.text || !A.live || !A.syl) { fprintf(stderr, "out of memory\n"); return 2; }
    if (kl_compile(&L, NULL, &A, &out) != KL_COMPILE_OK) return 1;

    mix_n     = kl_render_samples_for(out.total_ms, 22050.0);
    sched_cap = kl_render_need_sched(&out);
    memset(&R, 0, sizeof R);
    mix              = calloc(mix_n ? mix_n : 1, sizeof *mix);
    loud             = calloc(mix_n ? mix_n : 1, sizeof *loud);
    R.scratch        = calloc(mix_n ? mix_n : 1, sizeof *R.scratch);
    R.scratch_cap    = mix_n;
    R.at_sample      = calloc(sched_cap ? sched_cap : 1, sizeof *R.at_sample);
    R.transition_len = calloc(sched_cap ? sched_cap : 1, sizeof *R.transition_len);
    R.sched_cap      = sched_cap;
    if (!mix || !loud || !R.scratch || !R.at_sample || !R.transition_len) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    if (kl_render_mix(&out, 22050.0, mix, mix_n, &R) != KL_RENDER_OK) return 1;

    /* Float32Array.from(buf, v => v * 3): the multiply is in double and the
     * store rounds to single, exactly as everywhere else. */
    for (i = 0; i < mix_n; i++) loud[i] = (float)((double)mix[i] * 3.0);

    put_u32(7);

    meta.software = NULL; meta.comment = NULL;
    emit_wav(mix, mix_n, KL_WAV_PEAK_NORMALIZE, NULL);                    /* plain */

    meta.software = "klattsch-goldens"; meta.comment = "HH AH L OW";
    emit_wav(mix, mix_n, KL_WAV_PEAK_NORMALIZE, &meta);                   /* withMeta */

    emit_wav(mix, mix_n, 0.0, NULL);                                      /* noNormalize */

    meta.software = NULL; meta.comment = "odd";
    emit_wav(mix, mix_n, KL_WAV_PEAK_NORMALIZE, &meta);                   /* oddComment */

    emit_wav(loud, mix_n, 0.0, NULL);                                     /* clipped */

    emit_wav(mix, 0, KL_WAV_PEAK_NORMALIZE, NULL);                        /* empty */

    meta.software = ""; meta.comment = "x";
    emit_wav(mix, mix_n, KL_WAV_PEAK_NORMALIZE, &meta);                   /* emptySoftware */

    free(loud); free(mix);
    free(R.scratch); free(R.at_sample); free(R.transition_len);
    free(A.voices); free(A.events); free(A.spans); free(A.extras);
    free(A.phrases); free(A.warnings); free(A.text); free(A.live);
    free((void *)A.syl);
    free(L.source); free(L.tokens); free(L.arena);
    return 0;
}

/* --- the corpus ----------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *path;
    double rate;
    size_t blob_len = 0, p;
    unsigned char *blob;
    uint32_t n_cases, c;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (argc >= 2 && strcmp(argv[1], "--round-sweep") == 0) {
        round_sweep();
        return ferror(stdout) ? 1 : 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--tofixed-sweep") == 0) {
        tofixed_sweep();
        return ferror(stdout) ? 1 : 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--wav-goldens") == 0) {
        int rc = wav_goldens();
        if (rc) return rc;
        return ferror(stdout) ? 1 : 0;
    }

    if (argc < 3) {
        fprintf(stderr, "usage: kl_wav_dump <cases-compile.bin> <sample-rate>\n");
        fprintf(stderr, "       kl_wav_dump --round-sweep\n");
        fprintf(stderr, "       kl_wav_dump --tofixed-sweep\n");
        fprintf(stderr, "       kl_wav_dump --wav-goldens\n");
        return 2;
    }
    path = argv[1];
    rate = atof(argv[2]);
    if (!(rate > 0.0)) { fprintf(stderr, "bad sample rate %s\n", argv[2]); return 2; }

    blob = slurp(path, &blob_len);
    if (!blob) { fprintf(stderr, "cannot read %s\n", path); return 2; }

    n_cases = rd_u32(blob, 0);
    p = 4;
    put_u32(n_cases);

    for (c = 0; c < n_cases; c++) {
        uint32_t text_len, bank_len, engine_len, n_opt_extras, i;
        const char *text_raw;
        char *text = NULL, *bank = NULL, *engine = NULL;
        kl_compile_opts opts;
        kl_opt_extra *opt_extras = NULL;
        char **opt_extra_keys = NULL;
        size_t need_src, need_tok, need_arena;
        kl_token_list L;
        kl_compile_arena A;
        kl_compiled out;
        kl_render_arena R;
        kl_wav_meta meta;
        float *mix;
        unsigned char *wav;
        size_t mix_n, wav_cap, wav_len, sched_cap;
        double gain = 1.0;
        int rc;

        text_len = rd_u32(blob, p); p += 4;
        text_raw = (const char *)blob + p;
        text = dup_span(blob, p, text_len);     /* NUL-terminated, for the ICMT field */
        p += text_len;
        if (!text) { fprintf(stderr, "out of memory\n"); return 2; }

        memset(&opts, 0, sizeof opts);
        opts.present = rd_u32(blob, p); p += 4;
        for (i = 0; i < (uint32_t)KL_OPT_COUNT; i++) { opts.value[i] = rd_f64(blob, p); p += 8; }

        bank_len = rd_u32(blob, p); p += 4;
        if (bank_len) { bank = dup_span(blob, p, bank_len); opts.bank = bank; }
        p += bank_len;

        engine_len = rd_u32(blob, p); p += 4;
        if (engine_len) { engine = dup_span(blob, p, engine_len); opts.engine = engine; }
        p += engine_len;

        n_opt_extras = rd_u32(blob, p); p += 4;
        if (n_opt_extras) {
            opt_extras     = calloc(n_opt_extras, sizeof *opt_extras);
            opt_extra_keys = calloc(n_opt_extras, sizeof *opt_extra_keys);
            if (!opt_extras || !opt_extra_keys) { fprintf(stderr, "out of memory\n"); return 2; }
        }
        for (i = 0; i < n_opt_extras; i++) {
            uint32_t klen = rd_u32(blob, p); p += 4;
            opt_extra_keys[i] = dup_span(blob, p, klen); p += klen;
            opt_extras[i].key = opt_extra_keys[i];
            opt_extras[i].value = rd_f64(blob, p); p += 8;
        }
        opts.extras   = opt_extras;
        opts.n_extras = n_opt_extras;

        /* --- tokenize ---------------------------------------------------- */

        kl_token_need(text_len, &need_src, &need_tok, &need_arena);
        memset(&L, 0, sizeof L);
        L.source = malloc(need_src * sizeof(uint16_t));
        L.tokens = malloc(need_tok * sizeof(kl_token));
        L.arena  = malloc(need_arena);
        if (!L.source || !L.tokens || !L.arena) { fprintf(stderr, "out of memory\n"); return 2; }
        L.source_cap = need_src;
        L.tokens_cap = need_tok;
        L.arena_cap  = need_arena;

        if (kl_tokenize(text_raw, text_len, &L) != KL_TOKEN_OK) {
            fprintf(stderr, "tokenize overflow on case %u\n", c);
            return 1;
        }

        /* --- compile ----------------------------------------------------- */

        kl_compile_need(&L, &opts, &A);
        A.voices   = calloc(A.voices_cap   ? A.voices_cap   : 1, sizeof *A.voices);
        A.events   = calloc(A.events_cap   ? A.events_cap   : 1, sizeof *A.events);
        A.spans    = calloc(A.events_cap   ? A.events_cap   : 1, sizeof *A.spans);
        A.extras   = calloc(A.extras_cap   ? A.extras_cap   : 1, sizeof *A.extras);
        A.phrases  = calloc(A.phrases_cap  ? A.phrases_cap  : 1, sizeof *A.phrases);
        A.warnings = calloc(A.warnings_cap ? A.warnings_cap : 1, sizeof *A.warnings);
        A.text     = calloc(A.text_cap     ? A.text_cap     : 1, 1);
        A.live     = calloc(A.live_cap     ? A.live_cap     : 1, sizeof *A.live);
        A.syl      = calloc(A.syl_cap      ? A.syl_cap      : 1, sizeof *A.syl);
        if (!A.voices || !A.events || !A.spans || !A.extras || !A.phrases
            || !A.warnings || !A.text || !A.live || !A.syl) {
            fprintf(stderr, "out of memory\n");
            return 2;
        }

        rc = kl_compile(&L, &opts, &A, &out);
        if (rc != KL_COMPILE_OK) {
            fprintf(stderr, "compile failed (%d) on case %u\n", rc, c);
            return 1;
        }

        /* --- render, mix, encode ----------------------------------------- */

        mix_n     = kl_render_samples_for(out.total_ms, rate);
        sched_cap = kl_render_need_sched(&out);

        memset(&R, 0, sizeof R);
        mix            = calloc(mix_n ? mix_n : 1, sizeof *mix);
        R.scratch      = calloc(mix_n ? mix_n : 1, sizeof *R.scratch);
        R.scratch_cap  = mix_n;
        R.at_sample      = calloc(sched_cap ? sched_cap : 1, sizeof *R.at_sample);
        R.transition_len = calloc(sched_cap ? sched_cap : 1, sizeof *R.transition_len);
        R.sched_cap      = sched_cap;
        if (!mix || !R.scratch || !R.at_sample || !R.transition_len) {
            fprintf(stderr, "out of memory\n");
            return 2;
        }

        if (kl_render_mix(&out, rate, mix, mix_n, &R) != KL_RENDER_OK) {
            fprintf(stderr, "render overflow on case %u\n", c);
            return 1;
        }

        meta.software = KL_WAV_SOFTWARE;
        meta.comment  = text;
        wav_cap = kl_wav_size(mix_n, &meta);
        wav = malloc(wav_cap);
        if (!wav) { fprintf(stderr, "out of memory\n"); return 2; }

        wav_len = kl_wav_encode(mix, mix_n, (uint32_t)rate, KL_WAV_PEAK_NORMALIZE,
                                &meta, wav, wav_cap, &gain);
        if (wav_len != wav_cap) {
            fprintf(stderr, "encode wrote %zu of %zu bytes on case %u\n",
                    wav_len, wav_cap, c);
            return 1;
        }

        put_u32((uint32_t)wav_len);
        put_f64(gain);
        fwrite(wav, 1, wav_len, stdout);

        free(wav); free(mix);
        free(R.scratch); free(R.at_sample); free(R.transition_len);
        free(A.voices); free(A.events); free(A.spans); free(A.extras);
        free(A.phrases); free(A.warnings); free(A.text); free(A.live);
        free((void *)A.syl);
        free(L.source); free(L.tokens); free(L.arena);
        for (i = 0; i < n_opt_extras; i++) free(opt_extra_keys[i]);
        free(opt_extras); free(opt_extra_keys);
        free(bank); free(engine); free(text);
    }

    free(blob);
    return ferror(stdout) ? 1 : 0;
}
