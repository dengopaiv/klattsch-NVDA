/* Standalone WAV renderer.
 *
 *   klattsch "HH AH L OW" hello.wav
 *   klattsch "b140 AY+30 . AY-30" sweep.wav
 *
 * Translated from bin/klattsch.mjs, line for line, including the two lines it
 * prints to stderr. klattsch is Tony Gies's work; a translation of someone's
 * program is still their program, and the ISFT field this writes into every
 * file says so.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 *
 * This is the first thing in the repository that is a program rather than a
 * verified library, so what it is *not* is worth saying: it takes a phoneme
 * string and an output path and nothing else, because that is what the
 * JavaScript takes. Options for the bank, the sample rate and the engine
 * extras belong to the generator of phase 4, not here -- the corpus is driven
 * through csrc/tools/kl_wav_dump.c instead, and tools/verify-stage6.mjs
 * proves the two agree by running both real programs.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "kl_compile.h"
#include "kl_fmt.h"
#include "kl_render.h"
#include "kl_token.h"
#include "kl_wav.h"

#define SAMPLE_RATE 48000.0

static int fail_oom(void)
{
    fprintf(stderr, "out of memory\n");
    return 1;
}

/* Open the output file. On Windows the path is held as UTF-8 like everything
 * else here, and fopen would put it through the ANSI codepage -- so it goes
 * back to UTF-16 for the one call that touches the filesystem. Node's
 * writeFileSync takes a UTF-16 path, so this is what matches it. */
static FILE *open_out(const char *path_u8)
{
#ifdef _WIN32
    FILE *f = NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, path_u8, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = malloc((size_t)n * sizeof *w);
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, path_u8, -1, w, n) > 0)
        f = _wfopen(w, L"wb");
    free(w);
    return f;
#else
    return fopen(path_u8, "wb");
#endif
}

static int run(const char *text, const char *out_path)
{
    size_t text_len = strlen(text);
    size_t need_src, need_tok, need_arena;
    kl_token_list L;
    kl_compile_arena A;
    kl_compiled compiled;
    kl_render_arena R;
    kl_wav_meta meta;
    float *mix = NULL;
    unsigned char *wav = NULL;
    size_t mix_n, sched_cap, wav_cap, wav_len;
    double gain = 1.0;
    FILE *f;
    char kb[32], secs[32], g[32];
    int rc;

    kl_token_need(text_len, &need_src, &need_tok, &need_arena);
    memset(&L, 0, sizeof L);
    L.source = malloc(need_src * sizeof(uint16_t));
    L.tokens = malloc(need_tok * sizeof(kl_token));
    L.arena  = malloc(need_arena);
    if (!L.source || !L.tokens || !L.arena) return fail_oom();
    L.source_cap = need_src;
    L.tokens_cap = need_tok;
    L.arena_cap  = need_arena;

    if (kl_tokenize(text, text_len, &L) != KL_TOKEN_OK) {
        fprintf(stderr, "input too long\n");
        return 1;
    }

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
        || !A.warnings || !A.text || !A.live || !A.syl) return fail_oom();

    rc = kl_compile(&L, NULL, &A, &compiled);
    if (rc != KL_COMPILE_OK) {
        fprintf(stderr, "compile failed (%d)\n", rc);
        return 1;
    }

    /* `if (warnings.length) console.error('warnings: ' + warnings.join(', '))`
     * -- the warnings of every section, in section order. */
    if (compiled.n_warnings) {
        size_t vi, k, printed = 0;
        fputs("warnings: ", stderr);
        for (vi = 0; vi < compiled.n_voices; vi++) {
            const kl_voice *v = &compiled.voices[vi];
            for (k = 0; k < v->n_warnings; k++) {
                if (printed++) fputs(", ", stderr);
                fwrite(v->warnings[k].text, 1, v->warnings[k].len, stderr);
            }
        }
        fputc('\n', stderr);
    }

    mix_n     = kl_render_samples_for(compiled.total_ms, SAMPLE_RATE);
    sched_cap = kl_render_need_sched(&compiled);

    memset(&R, 0, sizeof R);
    mix              = calloc(mix_n ? mix_n : 1, sizeof *mix);
    R.scratch        = calloc(mix_n ? mix_n : 1, sizeof *R.scratch);
    R.scratch_cap    = mix_n;
    R.at_sample      = calloc(sched_cap ? sched_cap : 1, sizeof *R.at_sample);
    R.transition_len = calloc(sched_cap ? sched_cap : 1, sizeof *R.transition_len);
    R.sched_cap      = sched_cap;
    if (!mix || !R.scratch || !R.at_sample || !R.transition_len) return fail_oom();

    if (kl_render_mix(&compiled, SAMPLE_RATE, mix, mix_n, &R) != KL_RENDER_OK) {
        fprintf(stderr, "render failed\n");
        return 1;
    }

    meta.software = KL_WAV_SOFTWARE;
    meta.comment  = text;
    wav_cap = kl_wav_size(mix_n, &meta);
    wav = malloc(wav_cap);
    if (!wav) return fail_oom();

    wav_len = kl_wav_encode(mix, mix_n, (uint32_t)SAMPLE_RATE, KL_WAV_PEAK_NORMALIZE,
                            &meta, wav, wav_cap, &gain);
    if (wav_len == 0) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }

    f = open_out(out_path);
    if (!f) {
        fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }
    if (fwrite(wav, 1, wav_len, f) != wav_len || fclose(f) != 0) {
        fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }

    /* toFixed, not printf: both of these ties are reachable. See kl_fmt.h. */
    kl_to_fixed(kb,   sizeof kb,   (double)wav_len / 1024.0, 0);
    kl_to_fixed(secs, sizeof secs, compiled.total_ms / 1000.0, 2);
    kl_to_fixed(g,    sizeof g,    gain, 2);
    fprintf(stderr, "wrote %s: %s KB, %ss, normalize gain %sx\n",
            out_path, kb, secs, g);

    free(wav); free(mix);
    free(R.scratch); free(R.at_sample); free(R.transition_len);
    free(A.voices); free(A.events); free(A.spans); free(A.extras);
    free(A.phrases); free(A.warnings); free(A.text); free(A.live);
    free((void *)A.syl);
    free(L.source); free(L.tokens); free(L.arena);
    return 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: klattsch <phoneme-string> [output.wav]\n");
    fprintf(stderr, "  e.g. klattsch \"HH AH L OW\" hello.wav\n");
    return 1;
}

#ifdef _WIN32

/* wmain, because the engine speaks UTF-8 and Windows hands main() the command
 * line in the system ANSI codepage. A phoneme string is ASCII, but the text
 * also becomes the ICMT comment of the file, and the corpus has cases whose
 * text is not -- normalize/fullwidth among them. Reading the arguments as
 * UTF-16 and converting once is the only way that round-trips. */
static char *to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) return NULL;
    s = malloc((size_t)n);
    if (!s) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
        free(s);
        return NULL;
    }
    return s;
}

int wmain(int argc, wchar_t **wargv)
{
    char *text, *out_path;
    int rc;

    if (argc < 2) return usage();
    text = to_utf8(wargv[1]);
    if (!text) return fail_oom();
    /* `const [, , text, outPath = 'klattsch.wav'] = process.argv` and
     * `if (!text)`: an empty string is falsy in JavaScript, so it is usage. */
    if (!text[0]) { free(text); return usage(); }

    out_path = (argc >= 3) ? to_utf8(wargv[2]) : NULL;
    rc = run(text, out_path ? out_path : "klattsch.wav");
    free(text);
    free(out_path);
    return rc;
}

#else

int main(int argc, char **argv)
{
    if (argc < 2 || !argv[1][0]) return usage();
    return run(argv[1], (argc >= 3) ? argv[2] : "klattsch.wav");
}

#endif
