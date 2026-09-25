/* kl_text_dump -- run the text front end over lines of stdin.
 *
 *   kl_text_dump --nrl     pass 1 alone: the rules' raw ARPABET
 *   kl_text_dump --word    one word per line: phonemes and stress
 *   kl_text_dump --source  text to klattsch source, with the contour
 *   kl_text_dump --spell   spelled out, character by character
 *   kl_text_dump --source --cap N
 *                          the same into an N-byte buffer, printed as
 *                          "<returned length> <what fit>", so the verifier
 *                          can check how a short buffer is cut
 *   kl_text_dump --source --base HZ
 *                          the contour sized for a base pitch of HZ, as the
 *                          generator does (tools/verify-gui.mjs)
 *
 * One output line per input line, LF, in binary mode on every platform so the
 * bytes are the same wherever it runs. Input is UTF-8; a trailing CR is
 * dropped. tools/verify-text.mjs and tools/measure-text.mjs drive it.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Päiv Dengo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "kl_text.h"

#define LINE_MAX_BYTES 65536

int main(int argc, char **argv)
{
    static kl_text_ctx ctx;
    static char line[LINE_MAX_BYTES];
    static char out[LINE_MAX_BYTES * 8];
    enum { NRL, WORD, SOURCE, SPELL } mode;
    size_t cap = sizeof out;
    int show_len = 0, i;
    kl_text_opts opts;

    opts.base_f0 = 0.0;   /* the front end's default, 120 Hz */
    if (argc < 2) {
        fprintf(stderr, "usage: kl_text_dump --nrl|--word|--source|--spell"
                        " [--cap N] [--base HZ]\n");
        return 2;
    }
    for (i = 2; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--cap") == 0) {
            cap = (size_t)strtoul(argv[i + 1], NULL, 10);
            if (cap > sizeof out) cap = sizeof out;
            show_len = 1;
        } else if (strcmp(argv[i], "--base") == 0) {
            opts.base_f0 = strtod(argv[i + 1], NULL);
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 2;
        }
    }
    if (i != argc) {
        fprintf(stderr, "option without a value: %s\n", argv[i]);
        return 2;
    }
    if (strcmp(argv[1], "--nrl") == 0) mode = NRL;
    else if (strcmp(argv[1], "--word") == 0) mode = WORD;
    else if (strcmp(argv[1], "--source") == 0) mode = SOURCE;
    else if (strcmp(argv[1], "--spell") == 0) mode = SPELL;
    else {
        fprintf(stderr, "unknown mode: %s\n", argv[1]);
        return 2;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = '\0';
        if (n && line[n - 1] == '\r') line[--n] = '\0';
        size_t len = 0;
        switch (mode) {
        case NRL:    len = kl_text_nrl(&ctx, line, out, cap); break;
        case WORD:   len = kl_text_word(&ctx, line, out, cap); break;
        case SOURCE: len = kl_text_to_source(&ctx, line, &opts, out, cap); break;
        case SPELL:  len = kl_text_spell(&ctx, line, out, cap); break;
        }
        if (show_len)
            fprintf(stdout, "%lu ", (unsigned long)len);
        if (cap > 0)
            fputs(out, stdout);
        fputc('\n', stdout);
    }
    return 0;
}
