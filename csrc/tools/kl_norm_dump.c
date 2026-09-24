/* kl_norm_dump -- dump kl_norm's output for the stage 4 verifier.
 *
 *   kl_norm_dump --allcp      every code point 0..0x10FFFF, one at a time
 *   kl_norm_dump --file F     length-prefixed UTF-8 strings from F
 *
 * Binary on stdout. --allcp is the exhaustive half of the normalization
 * check: 1,112,064 code points, each normalized on its own and compared
 * against String.prototype.normalize('NFKC') plus the two replace() passes.
 * A singleton table is exactly the thing an exhaustive single-code-point
 * test can prove, which is why the test is shaped this way.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "kl_norm.h"

static void put_u32(uint32_t v)
{
    unsigned char b[4] = { (unsigned char)(v & 0xFFu), (unsigned char)((v >> 8) & 0xFFu),
                           (unsigned char)((v >> 16) & 0xFFu), (unsigned char)((v >> 24) & 0xFFu) };
    fwrite(b, 1, 4, stdout);
}

static void put_u16(uint16_t v)
{
    unsigned char b[2] = { (unsigned char)(v & 0xFFu), (unsigned char)((v >> 8) & 0xFFu) };
    fwrite(b, 1, 2, stdout);
}

/* Encode one code point as UTF-8, the way a caller would hand it to us. */
static size_t cp_to_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80u) { out[0] = (char)cp; return 1; }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (argc >= 2 && strcmp(argv[1], "--allcp") == 0) {
        static uint16_t buf[64];
        char in[8];
        uint32_t count = 0;
        for (uint32_t cp = 0; cp <= 0x10FFFFu; cp++) if (cp < 0xD800u || cp > 0xDFFFu) count++;
        put_u32(count);
        for (uint32_t cp = 0; cp <= 0x10FFFFu; cp++) {
            if (cp >= 0xD800u && cp <= 0xDFFFu) continue;
            size_t n_in = cp_to_utf8(cp, in);
            size_t n = kl_normalize_utf8(in, n_in, buf, sizeof buf / sizeof buf[0]);
            if (n == KL_NORM_OVERFLOW) { fprintf(stderr, "overflow at U+%04X\n", cp); return 1; }
            put_u16((uint16_t)n);
            for (size_t k = 0; k < n; k++) put_u16(buf[k]);
        }
        return ferror(stdout) ? 1 : 0;
    }

    if (argc >= 3 && strcmp(argv[1], "--file") == 0) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 4) { fclose(f); return 2; }
        unsigned char *blob = malloc((size_t)sz);
        if (!blob) { fclose(f); return 2; }
        if (fread(blob, 1, (size_t)sz, f) != (size_t)sz) { free(blob); fclose(f); return 2; }
        fclose(f);

        size_t p = 0;
        uint32_t n_str = (uint32_t)blob[0] | ((uint32_t)blob[1] << 8)
                       | ((uint32_t)blob[2] << 16) | ((uint32_t)blob[3] << 24);
        p = 4;
        put_u32(n_str);
        for (uint32_t s = 0; s < n_str; s++) {
            uint32_t len = (uint32_t)blob[p] | ((uint32_t)blob[p+1] << 8)
                         | ((uint32_t)blob[p+2] << 16) | ((uint32_t)blob[p+3] << 24);
            p += 4;
            size_t cap = kl_norm_bound(len);
            uint16_t *out = malloc(cap * sizeof(uint16_t));
            if (!out) { free(blob); return 2; }
            size_t n = kl_normalize_utf8((const char *)blob + p, len, out, cap);
            if (n == KL_NORM_OVERFLOW) { fprintf(stderr, "overflow on string %u\n", s); return 1; }
            put_u32((uint32_t)n);
            for (size_t k = 0; k < n; k++) put_u16(out[k]);
            free(out);
            p += len;
        }
        free(blob);
        return ferror(stdout) ? 1 : 0;
    }

    fprintf(stderr, "usage: kl_norm_dump --allcp | --file <blob>\n");
    return 2;
}
