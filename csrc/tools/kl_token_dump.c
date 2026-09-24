/* kl_token_dump -- dump kl_token's output for the stage 4 verifier.
 *
 *   kl_token_dump <blob>            tokenize every string in the blob
 *   kl_token_dump --numbers <blob>  parse every string as a decimal instead
 *
 * The blob is u32 count, then per string u32 byte length and the UTF-8 bytes;
 * tools/cases-to-bin.mjs writes it. Binary on stdout, little endian.
 *
 * --numbers exists because Number() is the one part of classifyPart() that is
 * arithmetic rather than shape, so it gets its own grid rather than being
 * tested only through the tokens that happen to carry a value.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "kl_token.h"
#include "kl_norm.h"

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
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 4) { fclose(f); return NULL; }
    unsigned char *b = malloc((size_t)sz);
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

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    int numbers = (argc >= 2 && strcmp(argv[1], "--numbers") == 0);
    const char *path = numbers ? (argc >= 3 ? argv[2] : NULL) : (argc >= 2 ? argv[1] : NULL);
    if (!path) { fprintf(stderr, "usage: kl_token_dump [--numbers] <blob>\n"); return 2; }

    size_t blob_len = 0;
    unsigned char *blob = slurp(path, &blob_len);
    if (!blob) { fprintf(stderr, "cannot read %s\n", path); return 2; }

    uint32_t n_cases = rd_u32(blob, 0);
    size_t p = 4;
    put_u32(n_cases);

    for (uint32_t c = 0; c < n_cases; c++) {
        uint32_t len = rd_u32(blob, p); p += 4;
        const char *text = (const char *)blob + p;

        if (numbers) {
            /* The grid is ASCII by construction, so the UTF-16 conversion is
             * a widening and nothing else. */
            uint16_t *u = malloc((len + 1) * sizeof(uint16_t));
            if (!u) return 2;
            for (uint32_t k = 0; k < len; k++) u[k] = (uint16_t)(unsigned char)text[k];
            put_f64(kl_parse_decimal(u, len));
            free(u);
            p += len;
            continue;
        }

        size_t need_src, need_tok, need_arena;
        kl_token_need(len, &need_src, &need_tok, &need_arena);

        kl_token_list L;
        L.source = malloc(need_src * sizeof(uint16_t));
        L.tokens = malloc(need_tok * sizeof(kl_token));
        L.arena  = malloc(need_arena);
        if (!L.source || !L.tokens || !L.arena) { fprintf(stderr, "out of memory\n"); return 2; }
        L.source_cap = need_src;
        L.tokens_cap = need_tok;
        L.arena_cap  = need_arena;

        if (kl_tokenize(text, len, &L) != KL_TOKEN_OK) {
            fprintf(stderr, "tokenize overflow on case %u\n", c);
            return 1;
        }

        /* The normalized source, as UTF-16 units, so the verifier can check
         * it against the `source` the reference returns alongside the tokens. */
        put_u32((uint32_t)L.source_len);
        for (size_t k = 0; k < L.source_len; k++) {
            put_u8((unsigned char)(L.source[k] & 0xFFu));
            put_u8((unsigned char)((L.source[k] >> 8) & 0xFFu));
        }

        put_u32((uint32_t)L.n_tokens);
        for (size_t k = 0; k < L.n_tokens; k++) {
            const kl_token *t = &L.tokens[k];
            const char *tn = kl_tok_type_name[t->type];
            put_str(tn, (uint32_t)strlen(tn));
            put_str(L.arena + t->code_off, t->code_len);
            put_str(L.arena + t->key_off,  t->key_len);
            put_str(L.arena + t->name_off, t->name_len);
            put_str(L.arena + t->text_off, t->text_len);
            put_u8(t->stressed);
            put_u8(t->transient);
            put_u8(t->relative);
            put_u8(t->reset);
            put_f64(t->value);
            put_f64(t->pitch_delta);
            put_f64(t->ms);
            put_i32(t->src_start);
            put_i32(t->src_end);
        }

        free(L.source); free(L.tokens); free(L.arena);
        p += len;
    }

    free(blob);
    return ferror(stdout) ? 1 : 0;
}
