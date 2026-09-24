/* kl_token.c -- translated from `tokenize()` and `classifyPart()` in
 * src/engine/sequencer.js.
 *
 * klattsch is Tony Gies's work and is MIT licensed; see LICENSE.
 *
 * The reference classifies with a cascade of regular expressions. They are
 * hand-matched here rather than dragged in with a regex engine: there are
 * nine of them, all anchored, all small, and a regex library would be a
 * dependency in the shipped artifact for nine patterns that fit on a screen.
 * The order of the cascade is load-bearing -- `[bank=x]` must be tried before
 * the generic `[key=value]` form -- so the order below is the reference's.
 */

#include "kl_token.h"
#include "kl_norm.h"

#include <math.h>
#include <string.h>

const char *const kl_tok_type_name[KL_TOK_TYPE_COUNT] = {
    "syllable_open", "syllable_close", "pause", "stress_mark",
    "bank_switch", "bank_reset", "engine_switch", "engine_reset",
    "directive", "phoneme", "unknown",
};

void kl_token_need(size_t len, size_t *source_units, size_t *max_tokens, size_t *arena_bytes)
{
    size_t units = kl_norm_bound(len);
    /* Twice, plus one. The upper half is tokenizer scratch: a block comment
     * inside a token splices the halves together, so the part has to be made
     * contiguous somewhere, and a token can be as long as the whole source.
     * Paying for it here keeps kl_tokenize() free of allocation. */
    if (source_units) *source_units = units * 2 + 1;
    /* A token needs at least one non-space unit, so there cannot be more
     * tokens than there are units. */
    if (max_tokens)   *max_tokens = units + 1;
    /* Each token stores its strings once, as UTF-8. Three bytes per UTF-16
     * unit covers the BMP; a surrogate pair is four bytes across two units,
     * so three per unit is the bound either way. */
    if (arena_bytes)  *arena_bytes = units * 3 + 1;
}

/* --- small helpers over UTF-16 ranges ------------------------------------- */

static int is_digit(uint16_t c)  { return c >= '0' && c <= '9'; }
static int is_upper(uint16_t c)  { return c >= 'A' && c <= 'Z'; }
static int is_lower(uint16_t c)  { return c >= 'a' && c <= 'z'; }
static int is_word(uint16_t c)   { return is_digit(c) || is_upper(c) || is_lower(c) || c == '_'; }
/* The class in [bank=...] and [engine=...]: [A-Za-z0-9_.\-] */
static int is_name(uint16_t c)   { return is_word(c) || c == '.' || c == '-'; }

static int eq_ascii(const uint16_t *s, size_t n, const char *lit)
{
    size_t k = 0;
    for (; k < n; k++)
        if (lit[k] == '\0' || (uint16_t)(unsigned char)lit[k] != s[k]) return 0;
    return lit[k] == '\0';
}

static int starts_with(const uint16_t *s, size_t n, const char *lit)
{
    for (size_t k = 0; lit[k]; k++)
        if (k >= n || (uint16_t)(unsigned char)lit[k] != s[k]) return 0;
    return 1;
}

/* --- Number(), over the forms the grammar allows -------------------------- */

/* The reference parses every numeric field with Number(), on strings a
 * regular expression has already constrained to  [+-]? digits [ . digits ].
 *
 * strtod() is not used, and the reason is not style: it takes the decimal
 * separator from the C locale, and this code is going to run inside NVDA on
 * machines where that separator is a comma. There, strtod("3.5") returns 3
 * and stops. A synthesizer whose pitch directives silently truncate on a
 * Finnish or German desktop is exactly the defect that never shows up on the
 * developer's machine.
 *
 * So the mantissa is accumulated as an integer and scaled by a power of ten.
 * While the mantissa fits in 2^53 and the scale is 10^0..10^22 -- both
 * exactly representable -- one IEEE division is correctly rounded and the
 * result is bit-identical to Number(). Outside that window the value is
 * computed the same way from the digits that fit and is no longer claimed to
 * be exact. docs/16-stage4-token.md measures where the window ends.
 */
static const double POW10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

double kl_parse_decimal(const uint16_t *s, size_t n)
{
    size_t i = 0;
    int neg = 0;
    uint64_t mant = 0;
    int frac = 0;        /* fractional digits that made it into the mantissa */
    int dropped = 0;     /* integer digits dropped for want of room          */
    int seen_point = 0;

    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = (s[i] == '-'); i++; }

    for (; i < n; i++) {
        if (s[i] == '.') { seen_point = 1; continue; }
        if (!is_digit(s[i])) break;
        /* Take the digit if the mantissa still fits in 2^53-1 afterwards.
         * Reserving room for a 9 instead of for *this* digit is a digit too
         * conservative, and drops the last digit of 9007199254740991. */
        if (mant <= (UINT64_C(9007199254740991) - (uint64_t)(s[i] - '0')) / 10) {
            mant = mant * 10u + (uint64_t)(s[i] - '0');
            if (seen_point) frac++;
        } else if (!seen_point) {
            dropped++;   /* still scales the value */
        }
        /* fractional digits past the mantissa are simply lost */
    }

    double v;
    int scale = frac - dropped;
    if (scale >= 0 && scale <= 22)        v = (double)mant / POW10[scale];
    else if (scale < 0 && -scale <= 22)   v = (double)mant * POW10[-scale];
    else                                  v = (double)mant * pow(10.0, (double)(-scale));
    return neg ? -v : v;
}

/* --- noteToHz ------------------------------------------------------------- */

static const int NOTE_SEMITONES[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A B C D E F G */

/* Matches /^([A-G])([b#]?)(-?\d+)$/ and returns the frequency, or -1 if the
 * shape is wrong. The reference's own `if (hz != null)` guard can never fire,
 * because its caller's regex has already pinned the shape; here the caller
 * genuinely does use the failure to fall through to the compact form. */
static double note_to_hz(const uint16_t *s, size_t n)
{
    if (n < 2 || !is_upper(s[0]) || s[0] > 'G') return -1.0;
    int semi = NOTE_SEMITONES[s[0] - 'A'];
    size_t i = 1;
    if (s[i] == '#' || s[i] == 'b') { semi += (s[i] == '#') ? 1 : -1; i++; }
    int oct_neg = 0;
    if (i < n && s[i] == '-') { oct_neg = 1; i++; }
    if (i >= n) return -1.0;
    long octave = 0;
    for (; i < n; i++) {
        if (!is_digit(s[i])) return -1.0;
        octave = octave * 10 + (long)(s[i] - '0');
    }
    if (oct_neg) octave = -octave;
    double midi = (double)((octave + 1) * 12 + semi);
    return 440.0 * pow(2.0, (midi - 69.0) / 12.0);
}

/* --- the string arena ----------------------------------------------------- */

static int arena_put_utf16(kl_token_list *L, const uint16_t *s, size_t n,
                           uint32_t *off, uint32_t *len)
{
    size_t wrote = kl_utf16_to_utf8(s, n, L->arena + L->arena_len, L->arena_cap - L->arena_len);
    if (wrote == KL_NORM_OVERFLOW) return KL_TOKEN_OVERFLOW;
    *off = (uint32_t)L->arena_len;
    *len = (uint32_t)wrote;
    L->arena_len += wrote;
    return KL_TOKEN_OK;
}

static int arena_put_ascii(kl_token_list *L, const char *lit,
                           uint32_t *off, uint32_t *len)
{
    size_t k = strlen(lit);
    if (L->arena_len + k > L->arena_cap) return KL_TOKEN_OVERFLOW;
    memcpy(L->arena + L->arena_len, lit, k);
    *off = (uint32_t)L->arena_len;
    *len = (uint32_t)k;
    L->arena_len += k;
    return KL_TOKEN_OK;
}

/* --- classifyPart --------------------------------------------------------- */

/* Returns 1 if a token was produced, 0 if the part is dropped (the reference
 * returns null for a bare `p`), or KL_TOKEN_OVERFLOW. */
static int classify(kl_token_list *L, const uint16_t *p, size_t n, kl_token *t)
{
    memset(t, 0, sizeof *t);
    t->src_start = -1;
    t->src_end = -1;

    if (n == 1 && p[0] == '(') { t->type = KL_TOK_SYLLABLE_OPEN;  return 1; }
    if (n == 1 && p[0] == ')') { t->type = KL_TOK_SYLLABLE_CLOSE; return 1; }

    /* PAUSE_MS. The reference writes `part in PAUSE_MS`, and `in` walks the
     * object's prototype chain: "toString", "valueOf", "constructor",
     * "hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable",
     * "toLocaleString" and "__proto__" all test true and classify as a pause
     * whose `ms` is a function. That is a defect, not a feature -- downstream
     * it turns atMs into a *string* by concatenation and totalMs into NaN,
     * with warnings still empty. This tests the three characters the table
     * actually holds. docs/16-stage4-token.md records the divergence, and
     * the corpus carries a case that pins it. */
    if (n == 1 && (p[0] == ',' || p[0] == ';' || p[0] == '.')) {
        t->type = KL_TOK_PAUSE;
        t->ms = (p[0] == ',') ? 100.0 : (p[0] == ';') ? 200.0 : 300.0;
        return 1;
    }

    if (n == 1 && (p[0] == '!' || p[0] == '\'')) { t->type = KL_TOK_STRESS_MARK; return 1; }

    /* /^\[bank=([A-Za-z0-9_.\-]+)\]$/ */
    if (n > 7 && starts_with(p, n, "[bank=") && p[n - 1] == ']') {
        size_t a = 6, b = n - 1, k = a;
        while (k < b && is_name(p[k])) k++;
        if (k == b) {
            t->type = KL_TOK_BANK_SWITCH;
            return arena_put_utf16(L, p + a, b - a, &t->name_off, &t->name_len) == KL_TOKEN_OK
                 ? 1 : KL_TOKEN_OVERFLOW;
        }
    }
    if (eq_ascii(p, n, "[bank]")) { t->type = KL_TOK_BANK_RESET; return 1; }

    /* /^\[engine=([A-Za-z0-9_.\-]+)\]$/ */
    if (n > 9 && starts_with(p, n, "[engine=") && p[n - 1] == ']') {
        size_t a = 8, b = n - 1, k = a;
        while (k < b && is_name(p[k])) k++;
        if (k == b) {
            t->type = KL_TOK_ENGINE_SWITCH;
            return arena_put_utf16(L, p + a, b - a, &t->name_off, &t->name_len) == KL_TOKEN_OK
                 ? 1 : KL_TOKEN_OVERFLOW;
        }
    }
    if (eq_ascii(p, n, "[engine]")) { t->type = KL_TOK_ENGINE_RESET; return 1; }

    /* /^\[(\w+)=(-?\d+(?:\.\d+)?)\]$/ */
    if (n >= 5 && p[0] == '[' && p[n - 1] == ']') {
        size_t k = 1;
        while (k < n - 1 && is_word(p[k])) k++;
        if (k > 1 && k < n - 1 && p[k] == '=') {
            size_t v0 = k + 1, v = v0;
            if (v < n - 1 && p[v] == '-') v++;
            size_t d0 = v;
            while (v < n - 1 && is_digit(p[v])) v++;
            int ok = (v > d0);
            if (ok && v < n - 1 && p[v] == '.') {
                v++;
                size_t f0 = v;
                while (v < n - 1 && is_digit(p[v])) v++;
                ok = (v > f0);
            }
            if (ok && v == n - 1) {
                t->type = KL_TOK_DIRECTIVE;
                if (arena_put_utf16(L, p + 1, k - 1, &t->key_off, &t->key_len) != KL_TOKEN_OK)
                    return KL_TOKEN_OVERFLOW;
                t->value = kl_parse_decimal(p + v0, (n - 1) - v0);
                return 1;
            }
        }
    }

    /* /^\[([A-Z]\w*)\]$/ -- a bare uppercase bracket resets an extended
     * directive. Lowercase brackets are deliberately not directives. */
    if (n >= 3 && p[0] == '[' && p[n - 1] == ']' && is_upper(p[1])) {
        size_t k = 2;
        while (k < n - 1 && is_word(p[k])) k++;
        if (k == n - 1) {
            t->type = KL_TOK_DIRECTIVE;
            if (arena_put_utf16(L, p + 1, n - 2, &t->key_off, &t->key_len) != KL_TOKEN_OK)
                return KL_TOKEN_OVERFLOW;
            t->reset = 1;
            return 1;
        }
    }

    /* /^(b)(=)?([A-G][b#]?-?\d+)$/ -- note names, e.g. b=C4, bA-1 */
    if (n >= 3 && p[0] == 'b') {
        size_t a = (p[1] == '=') ? 2u : 1u;
        double hz = note_to_hz(p + a, n - a);
        if (hz >= 0.0) {
            t->type = KL_TOK_DIRECTIVE;
            if (arena_put_ascii(L, "base", &t->key_off, &t->key_len) != KL_TOKEN_OK)
                return KL_TOKEN_OVERFLOW;
            t->value = hz;
            return 1;
        }
    }

    /* /^([a-z])(?:(=)?(([+-])?\d+(?:\.\d+)?))?$/ */
    if (n >= 1 && is_lower(p[0])) {
        static const char letters[] = "brpsvwmnhtg";
        static const char *const keys[] = {
            "base", "rate", "pause", "scale", "vibrato", "vibratoRate",
            "tremolo", "tremoloRate", "aspiration", "tilt", "effort",
        };
        const char *key = NULL;
        for (size_t k = 0; letters[k]; k++)
            if (letters[k] == (char)p[0]) { key = keys[k]; break; }

        size_t i = 1;
        int eq = 0, sign = 0, have_value = 0, shape_ok = 1;
        if (i < n && p[i] == '=') { eq = 1; i++; }
        size_t v0 = i;
        if (i < n && (p[i] == '+' || p[i] == '-')) { sign = 1; i++; }
        size_t d0 = i;
        while (i < n && is_digit(p[i])) i++;
        if (i > d0) {
            have_value = 1;
            if (i < n && p[i] == '.') {
                i++;
                size_t f0 = i;
                while (i < n && is_digit(p[i])) i++;
                if (i == f0) shape_ok = 0;
            }
        } else if (i != v0 || eq) {
            shape_ok = 0;          /* `b=` and `b+` are not the form */
        }
        if (i != n) shape_ok = 0;

        if (shape_ok && key) {
            if (!have_value) {
                /* Bare letter resets to the initial value -- except a bare
                 * `p`, which the reference drops outright. */
                if (strcmp(key, "pause") == 0) return 0;
                t->type = KL_TOK_DIRECTIVE;
                if (arena_put_ascii(L, key, &t->key_off, &t->key_len) != KL_TOKEN_OK)
                    return KL_TOKEN_OVERFLOW;
                t->reset = 1;
                return 1;
            }
            t->type = KL_TOK_DIRECTIVE;
            if (arena_put_ascii(L, key, &t->key_off, &t->key_len) != KL_TOKEN_OK)
                return KL_TOKEN_OVERFLOW;
            t->value = kl_parse_decimal(p + v0, n - v0);
            t->relative = (uint8_t)(!eq && sign);
            return 1;
        }
    }

    /* /^([A-Z]+)(['!])?(?:\(([+-]\d+(?:\.\d+)?)\)|([+-]\d+(?:\.\d+)?))?$/ */
    if (n >= 1 && is_upper(p[0])) {
        size_t i = 0;
        while (i < n && is_upper(p[i])) i++;
        size_t code_len = i;
        int stressed = 0;
        if (i < n && (p[i] == '\'' || p[i] == '!')) { stressed = 1; i++; }

        int transient = 0, have_delta = 0, shape_ok = 1;
        size_t d0 = 0, d1 = 0;
        if (i < n) {
            int paren = (p[i] == '(');
            if (paren) i++;
            size_t v0 = i;
            if (i < n && (p[i] == '+' || p[i] == '-')) i++; else shape_ok = 0;
            size_t g0 = i;
            while (i < n && is_digit(p[i])) i++;
            if (i == g0) shape_ok = 0;
            if (shape_ok && i < n && p[i] == '.') {
                i++;
                size_t f0 = i;
                while (i < n && is_digit(p[i])) i++;
                if (i == f0) shape_ok = 0;
            }
            size_t close_at = i;
            if (shape_ok && paren) {
                if (i < n && p[i] == ')') i++; else shape_ok = 0;
            }
            if (shape_ok && i == n) {
                have_delta = 1;
                transient = paren;
                d0 = v0; d1 = close_at;
            } else {
                shape_ok = 0;
            }
        }

        if (shape_ok) {
            t->type = KL_TOK_PHONEME;
            if (arena_put_utf16(L, p, code_len, &t->code_off, &t->code_len) != KL_TOKEN_OK)
                return KL_TOKEN_OVERFLOW;
            t->stressed = (uint8_t)stressed;
            t->pitch_delta = have_delta ? kl_parse_decimal(p + d0, d1 - d0) : 0.0;
            t->transient = (uint8_t)transient;
            return 1;
        }
    }

    t->type = KL_TOK_UNKNOWN;
    return arena_put_utf16(L, p, n, &t->text_off, &t->text_len) == KL_TOKEN_OK
         ? 1 : KL_TOKEN_OVERFLOW;
}

/* --- tokenize ------------------------------------------------------------- */

/* source.indexOf('*' '/', start + 2), with -1 mapped to the end. */
static size_t find_block_end(const uint16_t *s, size_t len, size_t start)
{
    for (size_t k = start + 2; k + 1 < len; k++)
        if (s[k] == '*' && s[k + 1] == '/') return k + 2;
    return len;
}

static int at_block_start(const uint16_t *s, size_t n, size_t i)
{
    return s[i] == '/' && i + 1 < n && s[i + 1] == '*';
}

int kl_tokenize(const char *utf8, size_t len, kl_token_list *L)
{
    L->source_len = kl_normalize_utf8(utf8, len, L->source, L->source_cap);
    if (L->source_len == KL_NORM_OVERFLOW) return KL_TOKEN_OVERFLOW;

    const uint16_t *s = L->source;
    size_t n = L->source_len, i = 0;
    L->n_tokens = 0;
    L->arena_len = 0;

    /* Scratch for one token, above the normalized source. kl_token_need()
     * sized the buffer for both; a token cannot be longer than the source. */
    if (L->source_cap < n * 2 + 1) return KL_TOKEN_OVERFLOW;
    uint16_t *part = L->source + n + 1;

    while (i < n) {
        uint16_t c = s[i];
        if (kl_is_space(c)) { i++; continue; }

        /* Line comment: `#` only at the start of the input or after
         * whitespace, so a `#` inside a token stays part of the token. */
        if (c == '#' && (i == 0 || kl_is_space(s[i - 1]))) {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (at_block_start(s, n, i)) { i = find_block_end(s, n, i); continue; }

        size_t src_start = i;
        size_t part_len = 0;
        /* A block comment inside a token splices the halves together, which
         * is why the part is copied out rather than pointed at. */
        while (i < n && !kl_is_space(s[i])) {
            if (at_block_start(s, n, i)) { i = find_block_end(s, n, i); continue; }
            part[part_len++] = s[i++];
        }
        size_t src_end = i;
        if (part_len == 0) continue;

        if (L->n_tokens >= L->tokens_cap) return KL_TOKEN_OVERFLOW;
        kl_token tok;
        int r = classify(L, part, part_len, &tok);
        if (r == KL_TOKEN_OVERFLOW) return KL_TOKEN_OVERFLOW;
        if (r == 0) continue;
        tok.src_start = (int32_t)src_start;
        tok.src_end   = (int32_t)src_end;

        if (tok.type == KL_TOK_STRESS_MARK) {
            for (size_t j = L->n_tokens; j-- > 0; )
                if (L->tokens[j].type == KL_TOK_PHONEME) { L->tokens[j].stressed = 1; break; }
            continue;
        }
        L->tokens[L->n_tokens++] = tok;
    }

    return KL_TOKEN_OK;
}
