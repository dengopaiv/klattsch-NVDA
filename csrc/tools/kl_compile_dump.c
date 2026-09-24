/* kl_compile_dump -- dump kl_compile's output for the stage 5 verifier.
 *
 *   kl_compile_dump <blob>
 *
 * The blob is goldens/cases-compile.bin, written by tools/cases-to-bin.mjs:
 * each case is its text plus the opts that compileSection reads, because 262
 * of the 714 corpus cases carry a bank, an engine or seeded extras and a
 * compiler driven only by text would never reach those paths.
 *
 * Raw fields go out, not digests. The verifier recomputes both digests on the
 * JavaScript side, which means a mismatch can be reported as "case X, voice
 * 0, event 3, F2" instead of as two hex strings that differ.
 *
 * Binary on stdout, little endian.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "kl_compile.h"
#include "kl_token.h"

static void put_u32(uint32_t v)
{
    unsigned char b[4] = { (unsigned char)(v & 0xFFu), (unsigned char)((v >> 8) & 0xFFu),
                           (unsigned char)((v >> 16) & 0xFFu), (unsigned char)((v >> 24) & 0xFFu) };
    fwrite(b, 1, 4, stdout);
}

static void put_i32(int32_t v) { put_u32((uint32_t)v); }
static void put_u8(unsigned char v) { fwrite(&v, 1, 1, stdout); }

static void put_f64(double v)
{
    unsigned char b[8];
    memcpy(b, &v, 8);            /* the host is little-endian everywhere this builds */
    fwrite(b, 1, 8, stdout);
}

static void put_str(const char *s, uint32_t len)
{
    put_u32(len);
    if (len) fwrite(s, 1, len, stdout);
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

/* A NUL-terminated copy of a counted slice of the blob, because opts.bank and
 * opts.engine cross the C API as C strings. */
static char *dup_span(const unsigned char *b, size_t p, uint32_t len)
{
    char *s = malloc((size_t)len + 1);
    if (!s) return NULL;
    if (len) memcpy(s, b + p, len);
    s[len] = '\0';
    return s;
}

int main(int argc, char **argv)
{
    const char *path;
    size_t blob_len = 0, p;
    unsigned char *blob;
    uint32_t n_cases, c;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    path = (argc >= 2) ? argv[1] : NULL;
    if (!path) { fprintf(stderr, "usage: kl_compile_dump <cases-compile.bin>\n"); return 2; }

    blob = slurp(path, &blob_len);
    if (!blob) { fprintf(stderr, "cannot read %s\n", path); return 2; }

    n_cases = rd_u32(blob, 0);
    p = 4;
    put_u32(n_cases);

    for (c = 0; c < n_cases; c++) {
        uint32_t text_len, bank_len, engine_len, n_opt_extras, i;
        const char *text;
        char *bank = NULL, *engine = NULL;
        kl_compile_opts opts;
        kl_opt_extra *opt_extras = NULL;
        char **opt_extra_keys = NULL;
        size_t need_src, need_tok, need_arena;
        kl_token_list L;
        kl_compile_arena A;
        kl_compiled out;
        size_t v;
        int rc;

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

        if (kl_tokenize(text, text_len, &L) != KL_TOKEN_OK) {
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

        /* --- emit -------------------------------------------------------- */

        put_f64(out.total_ms);
        put_u32((uint32_t)out.n_voices);

        /* Warnings, merged in section order -- the JS's voices.flatMap(). */
        put_u32((uint32_t)out.n_warnings);
        for (v = 0; v < out.n_voices; v++) {
            size_t k;
            for (k = 0; k < out.voices[v].n_warnings; k++)
                put_str(out.voices[v].warnings[k].text, out.voices[v].warnings[k].len);
        }

        for (v = 0; v < out.n_voices; v++) {
            const kl_voice *V = &out.voices[v];
            size_t k;

            put_f64(V->total_ms);
            put_u8(V->engine ? 1 : 0);
            put_str(V->engine ? V->engine : "", V->engine ? (uint32_t)V->engine_len : 0);

            put_u32((uint32_t)V->n_events);
            for (k = 0; k < V->n_events; k++) {
                const kl_event *e = &V->events[k];
                const kl_extra_span *sp = &V->spans[k];
                uint32_t j;

                put_f64(e->at_ms);
                put_f64(e->transition_ms);
                for (j = 0; j < (uint32_t)KL_PARAM_COUNT; j++) {
                    if (e->present & ((uint32_t)1u << j)) {
                        put_u8(1);
                        put_f64(e->value[j]);
                    } else {
                        put_u8(0);
                    }
                }
                put_u32(sp->count);
                for (j = 0; j < sp->count; j++) {
                    const kl_extra *x = &out.extra_pool[sp->first + j];
                    put_str(x->key, x->key_len);
                    put_f64(x->value);
                }
            }

            put_u32((uint32_t)V->n_phrases);
            for (k = 0; k < V->n_phrases; k++) {
                const kl_phrase *ph = &V->phrases[k];
                const char *kind = kl_tok_type_name[ph->kind];
                put_i32(ph->src_start);
                put_i32(ph->src_end);
                put_i32(ph->token_src_start);
                put_f64(ph->t_start_ms);
                put_f64(ph->t_end_ms);
                put_str(kind, (uint32_t)strlen(kind));
                put_str(ph->phoneme ? ph->phoneme : "", ph->phoneme_len);
            }
        }

        free(A.voices); free(A.events); free(A.spans); free(A.extras);
        free(A.phrases); free(A.warnings); free(A.text); free(A.live);
        free((void *)A.syl);
        free(L.source); free(L.tokens); free(L.arena);
        for (i = 0; i < n_opt_extras; i++) free(opt_extra_keys[i]);
        free(opt_extras); free(opt_extra_keys);
        free(bank); free(engine);
    }

    free(blob);
    return ferror(stdout) ? 1 : 0;
}
