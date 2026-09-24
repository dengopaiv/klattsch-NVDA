/* kl_norm.c -- translated from the `normalize()` step of
 * src/engine/sequencer.js. See kl_norm.h for what is and is not implemented,
 * and docs/16-stage4-token.md for the measurements behind it.
 *
 * klattsch is Tony Gies's work and is MIT licensed; see LICENSE.
 */

#include "kl_norm.h"

/* JavaScript's /\s/ -- ECMA-262 WhiteSpace plus LineTerminator. Worth
 * spelling out rather than reaching for isspace(): isspace() is locale
 * dependent and knows nothing above U+007F, and this set has seven members
 * above it. U+FEFF is in the set, which is a trap only avoided because the
 * reference strips it before tokenizing. */
int kl_is_space(uint16_t u)
{
    switch (u) {
    case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
    case 0x0020: case 0x00A0: case 0x1680:
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
    case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
    case 0x200A:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
    case 0xFEFF:
        return 1;
    default:
        return 0;
    }
}

size_t kl_norm_bound(size_t in_len)
{
    return in_len * 18u + 1u;   /* U+FDFA is the longest expansion, 18 units */
}

static const kl_norm_entry *lookup(uint32_t cp)
{
    size_t lo = 0, hi = kl_norm_table_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t k = kl_norm_table[mid].cp;
        if (k == cp) return &kl_norm_table[mid];
        if (k < cp) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static int is_zero_width(uint16_t u)
{
    for (size_t i = 0; i < kl_zero_width_len; i++)
        if (kl_zero_width[i] == u) return 1;
    return 0;
}

static int homoglyph(uint16_t u, char *out)
{
    for (size_t i = 0; i < kl_homoglyph_table_len; i++) {
        if (kl_homoglyph_table[i].from == u) { *out = kl_homoglyph_table[i].to; return 1; }
    }
    return 0;
}

/* Decode one UTF-8 sequence. Returns the code point and advances *i. An
 * invalid sequence yields U+FFFD and consumes one byte, which is the
 * "maximal subpart" behaviour the Unicode standard recommends and what
 * TextDecoder does. */
static uint32_t utf8_next(const unsigned char *s, size_t len, size_t *i)
{
    unsigned char c = s[*i];
    uint32_t cp;
    size_t need;

    if (c < 0x80u)                  { (*i)++; return c; }
    else if ((c & 0xE0u) == 0xC0u)  { cp = c & 0x1Fu; need = 1; }
    else if ((c & 0xF0u) == 0xE0u)  { cp = c & 0x0Fu; need = 2; }
    else if ((c & 0xF8u) == 0xF0u)  { cp = c & 0x07u; need = 3; }
    else                            { (*i)++; return 0xFFFDu; }

    /* Need bytes at *i+1 .. *i+need, so the sequence is truncated unless
     * *i + need is still inside the buffer. */
    if (*i + need >= len) { (*i)++; return 0xFFFDu; }
    for (size_t k = 1; k <= need; k++) {
        unsigned char cc = s[*i + k];
        if ((cc & 0xC0u) != 0x80u) { (*i)++; return 0xFFFDu; }
        cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
    }
    /* Overlong, surrogate, and out-of-range sequences are all invalid. */
    if ((need == 1 && cp < 0x80u) || (need == 2 && cp < 0x800u) || (need == 3 && cp < 0x10000u)
        || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
        (*i)++;
        return 0xFFFDu;
    }
    *i += need + 1;
    return cp;
}

static int emit_cp(uint32_t cp, uint16_t *out, size_t cap, size_t *n)
{
    if (cp < 0x10000u) {
        if (*n + 1 > cap) return 0;
        out[(*n)++] = (uint16_t)cp;
    } else {
        if (*n + 2 > cap) return 0;
        cp -= 0x10000u;
        out[(*n)++] = (uint16_t)(0xD800u + (cp >> 10));
        out[(*n)++] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
    }
    return 1;
}

size_t kl_normalize_utf8(const char *in, size_t in_len, uint16_t *out, size_t out_cap)
{
    const unsigned char *s = (const unsigned char *)in;
    size_t i = 0, n = 0;

    /* Pass 1 -- decode, and apply the NFKC singleton table. */
    while (i < in_len) {
        uint32_t cp = utf8_next(s, in_len, &i);
        const kl_norm_entry *e = lookup(cp);
        if (e) {
            if (n + e->len > out_cap) return KL_NORM_OVERFLOW;
            for (uint16_t k = 0; k < e->len; k++) out[n++] = kl_norm_pool[e->off + k];
        } else if (!emit_cp(cp, out, out_cap, &n)) {
            return KL_NORM_OVERFLOW;
        }
    }

    /* Pass 2 -- strip the zero-width set. Separate from pass 1 because the
     * reference is separate: an NFKC expansion that contained a zero-width
     * character would be stripped by the JS and must be stripped here. */
    size_t w = 0;
    for (size_t k = 0; k < n; k++) if (!is_zero_width(out[k])) out[w++] = out[k];
    n = w;

    /* Pass 3 -- homoglyphs, last, as in the reference. */
    for (size_t k = 0; k < n; k++) {
        char latin;
        if (homoglyph(out[k], &latin)) out[k] = (uint16_t)(unsigned char)latin;
    }

    return n;
}

size_t kl_utf16_to_utf8(const uint16_t *in, size_t in_len, char *out, size_t out_cap)
{
    size_t n = 0;
    for (size_t i = 0; i < in_len; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < in_len
            && in[i + 1] >= 0xDC00u && in[i + 1] <= 0xDFFFu) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (in[i + 1] - 0xDC00u);
            i++;
        } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;   /* unpaired surrogate, as Node's UTF-8 encoder does */
        }

        if (cp < 0x80u) {
            if (n + 1 > out_cap) return KL_NORM_OVERFLOW;
            out[n++] = (char)cp;
        } else if (cp < 0x800u) {
            if (n + 2 > out_cap) return KL_NORM_OVERFLOW;
            out[n++] = (char)(0xC0u | (cp >> 6));
            out[n++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            if (n + 3 > out_cap) return KL_NORM_OVERFLOW;
            out[n++] = (char)(0xE0u | (cp >> 12));
            out[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[n++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            if (n + 4 > out_cap) return KL_NORM_OVERFLOW;
            out[n++] = (char)(0xF0u | (cp >> 18));
            out[n++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            out[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[n++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    return n;
}
