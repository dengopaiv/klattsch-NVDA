/* kl_text.c -- the English text front end.  See kl_text.h for the shape and
 * docs/19-frontend-text.md for how each pass was measured.
 *
 * Pass 1 (letter to sound) is Votraxxion's src/ttv.c, stage one, lifted with
 * two changes and no others: every ARPABET character it writes records which
 * position of the working text produced it (pass 3 needs that to find a
 * word's suffix among its phonemes), and the identifiers are renamed.  The
 * output is unchanged, and tools/verify-text.mjs holds it to that.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Päiv Dengo
 * See NOTICE.md.
 */

#include "kl_text.h"

#include <string.h>

#include "kl_text_rules.h"

/* ---------------------------------------------------------------- classes --
 *
 * The character classes the NRL context syntax is written in.  `is_voiced` is
 * the set the rules spell `.`; it is not the phonetic voiced set, it is the
 * specific eleven letters the 1976 report chose.
 *
 * These test ASCII ranges directly rather than calling isalpha(): the working
 * text is upper-cased ASCII by the time a rule sees it, and under a locale
 * where isalpha() is true for a byte above 127, `KL_LTS_NRL_RULES[1 + (c -
 * 'A')]` would index past the end of a 27-entry array. */

static int is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static int is_lower(char c) { return c >= 'a' && c <= 'z'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alnum(char c) { return is_upper(c) || is_digit(c) || is_lower(c); }

static int is_vowel(char c)
{
    return c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U';
}

static int is_consonant(char c)
{
    return is_upper(c) && !is_vowel(c);
}

static int is_voiced(char c)
{
    return c != '\0' && strchr("BDVGJLMNRWZ", c) != NULL;
}

static int is_front_vowel(char c)
{
    return c == 'E' || c == 'I' || c == 'Y';
}

/* The rules spell a word boundary as a space, but they are applied to running
 * text where the boundary is just as likely to be a comma or a full stop.
 * Treating any non-alphanumeric as a boundary is what makes "hello," come out
 * the same as "hello". */
static int is_boundary(char c)
{
    return !is_alnum(c);
}

static char to_upper(char c)
{
    return is_lower(c) ? (char)(c - 'a' + 'A') : c;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

/* ---------------------------------------------------------- context match --
 *
 * Both contexts are matched outward from the cursor.  The left pattern is
 * stored in reverse reading order, so it is walked from its own end backwards
 * while the text walks backwards too; the right pattern is walked forwards.
 * `text` is the whole space-padded buffer and `pos` the index just outside the
 * matched letters, so both walks stay in bounds on the padding. */

static int match_left(const char *pattern, const char *text, int pos)
{
    int pat;
    if (*pattern == '\0')
        return 1;
    for (pat = (int)strlen(pattern) - 1; pat >= 0; pat--) {
        char p = pattern[pat];
        char t = pos >= 0 ? text[pos] : ' ';
        if (is_upper(p)) {
            if (p != t)
                return 0;
            pos--;
            continue;
        }
        switch (p) {
        case ' ':
            if (!is_boundary(t)) return 0;
            pos--;
            break;
        case '#':  /* one or more vowels */
            if (!is_vowel(t)) return 0;
            pos--;
            while (pos >= 0 && is_vowel(text[pos])) pos--;
            break;
        case ':':  /* zero or more consonants */
            while (pos >= 0 && is_consonant(text[pos])) pos--;
            break;
        case '^':  /* exactly one consonant */
            if (!is_consonant(t)) return 0;
            pos--;
            break;
        case '.':  /* a voiced consonant */
            if (!is_voiced(t)) return 0;
            pos--;
            break;
        case '+':  /* a front vowel */
            if (!is_front_vowel(t)) return 0;
            pos--;
            break;
        default:
            return 0;  /* '%' is right-context only; anything else is a typo */
        }
    }
    return 1;
}

static int match_right(const char *pattern, const char *text, int n, int pos)
{
    const char *pat;
    if (*pattern == '\0')
        return 1;
    for (pat = pattern; *pat; pat++) {
        char t = pos < n ? text[pos] : ' ';
        if (is_upper(*pat)) {
            if (*pat != t)
                return 0;
            pos++;
            continue;
        }
        switch (*pat) {
        case ' ':
            if (!is_boundary(t)) return 0;
            pos++;
            break;
        case '#':
            if (!is_vowel(t)) return 0;
            pos++;
            while (pos < n && is_vowel(text[pos])) pos++;
            break;
        case ':':
            while (pos < n && is_consonant(text[pos])) pos++;
            break;
        case '^':
            if (!is_consonant(t)) return 0;
            pos++;
            break;
        case '.':
            if (!is_voiced(t)) return 0;
            pos++;
            break;
        case '+':
            if (!is_front_vowel(t)) return 0;
            pos++;
            break;
        case '%': {
            /* A suffix: E, ER, ES, ED, ELY or ING.  The one context class that
             * consumes a variable, spelled-out string rather than a class. */
            char a1 = pos + 1 < n ? text[pos + 1] : ' ';
            char a2 = pos + 2 < n ? text[pos + 2] : ' ';
            if (t == 'E') {
                pos++;
                a1 = pos < n ? text[pos] : ' ';
                if (a1 == 'L') {
                    char nx = pos + 1 < n ? text[pos + 1] : ' ';
                    if (nx != 'Y') return 0;
                    pos += 2;
                } else if (a1 == 'R' || a1 == 'S' || a1 == 'D') {
                    pos++;
                }
                break;
            }
            if (t == 'I' && a1 == 'N' && a2 == 'G') {
                pos += 3;
                break;
            }
            return 0;
        }
        default:
            return 0;
        }
    }
    return 1;
}

/* ----------------------------------------------------------- ARPABET out --
 *
 * Every character carries the working-text position of the rule that wrote
 * it.  That is the one addition to Votraxxion's matcher, and it changes no
 * output: pass 3 reads it to tell which phonemes a word's suffix produced. */

static void arpa_putc(kl_text_ctx *x, char c)
{
    x->arpa_full++;
    if (x->arpa_len < KL_TEXT_ARPA - 1) {
        x->arpa_src[x->arpa_len] = x->cur_src;
        x->arpa[x->arpa_len++] = c;
    }
}

static void arpa_puts(kl_text_ctx *x, const char *s)
{
    for (; *s; s++)
        arpa_putc(x, *s);
}

/* A word and a trailing space, which is how every table entry is spoken. */
static void arpa_say(kl_text_ctx *x, const char *word)
{
    arpa_puts(x, word);
    arpa_putc(x, ' ');
}

/* ---------------------------------------------------------------- numbers --
 *
 * The shape is Wasser's saynum.c (1985, public domain), the reader that
 * travelled with these rules: scales down to billions, "and" before a remainder
 * under a hundred ("three thousand and five"), and 1100..1999 read in hundreds
 * ("nineteen hundred eighty four"), which is how years and most four-digit
 * figures are said aloud.  Ordinals put TH on whichever word ends the number.
 *
 * Where Votraxxion departs from Wasser it is for a screen reader's sake: a
 * digit run with a leading zero or more than twelve digits is an identifier,
 * not a quantity, and is read digit by digit; commas between groups of three
 * are thousands separators; and any ST/ND/RD/TH suffix makes an ordinal, where
 * Wasser matched the suffix against the last digit and so read "11th" as
 * "eleven T H". */

#define KL_NUMBER_DIGITS 12   /* longest run read as a quantity */

static void say_scale(kl_text_ctx *x, const char *word, int ordinal)
{
    arpa_puts(x, word);
    if (ordinal)
        arpa_puts(x, "TH");
    arpa_putc(x, ' ');
}

/* A value below 10^12 as words; `ordinal` applies to the final word only. */
static void say_number(kl_text_ctx *x, unsigned long long value, int ordinal)
{
    static const struct { unsigned long long unit; const char *const *word; }
        scales[] = { { 1000000000ULL, &KL_LTS_BILLION },
                     { 1000000ULL,    &KL_LTS_MILLION } };
    size_t i;

    for (i = 0; i < sizeof scales / sizeof scales[0]; i++) {
        if (value < scales[i].unit)
            continue;
        say_number(x, value / scales[i].unit, 0);
        value %= scales[i].unit;
        say_scale(x, *scales[i].word, ordinal && value == 0);
        if (value == 0)
            return;
        if (value < 100)
            arpa_say(x, KL_LTS_AND);
    }

    if ((value >= 1000 && value <= 1099) || value >= 2000) {
        say_number(x, value / 1000, 0);
        value %= 1000;
        say_scale(x, KL_LTS_THOUSAND, ordinal && value == 0);
        if (value == 0)
            return;
        if (value < 100)
            arpa_say(x, KL_LTS_AND);
    }

    if (value >= 100) {            /* up to 19, for 1100..1999 */
        arpa_say(x, KL_LTS_CARDINALS[value / 100]);
        value %= 100;
        say_scale(x, KL_LTS_HUNDRED, ordinal && value == 0);
        if (value == 0)
            return;
    }

    if (value >= 20) {             /* [20] is twenty, [21] thirty ... */
        int tens = 18 + (int)(value / 10);
        value %= 10;
        if (value == 0) {
            arpa_say(x, ordinal ? KL_LTS_ORDINALS[tens] : KL_LTS_CARDINALS[tens]);
            return;
        }
        arpa_say(x, KL_LTS_CARDINALS[tens]);
    }

    arpa_say(x, ordinal ? KL_LTS_ORDINALS[value] : KL_LTS_CARDINALS[value]);
}

static void say_digits(kl_text_ctx *x, const char *digits, int len)
{
    int i;
    for (i = 0; i < len; i++)
        arpa_say(x, KL_LTS_CARDINALS[digits[i] - '0']);
}

/* The integer part of a number starting at `pos`: a digit run, plus any
 * ",ddd" groups that follow it when the run could lead a thousands-separated
 * figure.  "1,000" joins; "1,2,3" and "12,34" stay a list.  The digits are
 * copied into `digits` (separators dropped) and the end position returned. */
static int scan_integer(const kl_text_ctx *x, int pos, int n,
                        char *digits, int *len, int cap)
{
    int end = pos, count = 0;

    while (end < n && is_digit(x->text[end])) {
        if (count < cap)
            digits[count] = x->text[end];
        count++;
        end++;
    }
    if (count <= 3) {
        while (end + 3 < n && x->text[end] == ',' &&
               is_digit(x->text[end + 1]) && is_digit(x->text[end + 2]) &&
               is_digit(x->text[end + 3]) &&
               !(end + 4 < n && is_digit(x->text[end + 4]))) {
            int k;
            for (k = 1; k <= 3; k++) {
                if (count < cap)
                    digits[count] = x->text[end + k];
                count++;
            }
            end += 4;
        }
    }
    *len = count;
    return end;
}

/* Is this run a quantity, and if so what is it?  Leading zeros and over-long
 * runs are identifiers -- a part number, a phone number, "007" -- and read
 * digit by digit. */
static int as_quantity(const char *digits, int len, unsigned long long *value)
{
    int i;
    if (len > KL_NUMBER_DIGITS || (len > 1 && digits[0] == '0'))
        return 0;
    *value = 0;
    for (i = 0; i < len; i++)
        *value = *value * 10 + (unsigned long long)(digits[i] - '0');
    return 1;
}

static int is_ordinal_suffix(const kl_text_ctx *x, int end, int n)
{
    char a, b;
    if (end + 1 >= n)
        return 0;
    a = x->text[end];
    b = x->text[end + 1];
    if (!((a == 'S' && b == 'T') || (a == 'N' && b == 'D') ||
          (a == 'R' && b == 'D') || (a == 'T' && b == 'H')))
        return 0;
    return end + 2 >= n || is_boundary(x->text[end + 2]);
}

/* ".5", ".2.3": each point and the digits after it, digit by digit.  Repeating
 * is what reads a version string -- "one point two point three" -- and it is
 * also what stops the full stop reaching the rules, where it is a sentence
 * pause. */
static int say_fraction(kl_text_ctx *x, int end, int n)
{
    while (end + 1 < n && x->text[end] == '.' && is_digit(x->text[end + 1])) {
        arpa_say(x, KL_LTS_POINT);
        end++;
        while (end < n && is_digit(x->text[end]))
            arpa_say(x, KL_LTS_CARDINALS[x->text[end++] - '0']);
    }
    return end;
}

/* A number at `pos`: cardinal, ordinal, decimal or identifier. */
static int read_number(kl_text_ctx *x, int pos, int n)
{
    char digits[KL_NUMBER_DIGITS + 1];
    unsigned long long value;
    int len;
    int end = scan_integer(x, pos, n, digits, &len, KL_NUMBER_DIGITS + 1);

    if (!as_quantity(digits, len, &value)) {
        if (len > KL_NUMBER_DIGITS) {
            /* Too long to have been copied; read it from the text instead,
             * skipping any separators the scan joined. */
            int i;
            for (i = pos; i < end; i++)
                if (is_digit(x->text[i]))
                    arpa_say(x, KL_LTS_CARDINALS[x->text[i] - '0']);
        } else {
            say_digits(x, digits, len);
        }
        return say_fraction(x, end, n);
    }

    if (is_ordinal_suffix(x, end, n)) {
        say_number(x, value, 1);
        return end + 2;
    }
    say_number(x, value, 0);
    return say_fraction(x, end, n);
}

/* "$5", "$1,200", "$4.20", "$0.99": dollars, and cents when the fraction is
 * exactly two digits.  Any other fraction is read as a decimal before the
 * currency word ("$1.5" is one point five dollars).  `pos` is at the '$'. */
static int read_money(kl_text_ctx *x, int pos, int n)
{
    char digits[KL_NUMBER_DIGITS + 1];
    unsigned long long dollars = 0, cents = 0;
    int len, end, has_cents = 0;

    end = scan_integer(x, pos + 1, n, digits, &len, KL_NUMBER_DIGITS + 1);
    if (len > KL_NUMBER_DIGITS || !as_quantity(digits, len, &dollars)) {
        /* "$007" is not an amount anyone says; read the digits. */
        return read_number(x, pos + 1, n);
    }

    if (end + 2 < n && x->text[end] == '.' &&
        is_digit(x->text[end + 1]) && is_digit(x->text[end + 2]) &&
        !(end + 3 < n && is_digit(x->text[end + 3]))) {
        cents = (unsigned long long)((x->text[end + 1] - '0') * 10 +
                                     (x->text[end + 2] - '0'));
        has_cents = 1;
    }

    if (has_cents) {
        if (dollars > 0 || cents == 0) {
            say_number(x, dollars, 0);
            arpa_say(x, dollars == 1 ? KL_LTS_DOLLAR : KL_LTS_DOLLARS);
        }
        if (cents > 0) {
            if (dollars > 0)
                arpa_say(x, KL_LTS_AND);
            say_number(x, cents, 0);
            arpa_say(x, cents == 1 ? KL_LTS_CENT : KL_LTS_CENTS);
        }
        return end + 3;
    }

    say_number(x, dollars, 0);
    len = end;
    end = say_fraction(x, end, n);
    arpa_say(x, dollars == 1 && end == len ? KL_LTS_DOLLAR : KL_LTS_DOLLARS);
    return end;
}

/* Replace every occurrence of `from` with `to`, in place.  The scan resumes
 * just past the text that was written, so a replacement containing the
 * pattern is not rescanned. */
static void replace_all(kl_text_ctx *x, const char *from, const char *to)
{
    int from_len = (int)strlen(from);
    int to_len = (int)strlen(to);
    int at = 0;

    if (from_len == 0)
        return;

    while (at <= x->text_len - from_len) {
        char *hit = strstr(x->text + at, from);
        int pos, tail;
        if (!hit)
            return;
        pos = (int)(hit - x->text);

        tail = x->text_len - pos - from_len;
        if (pos + to_len + tail >= KL_TEXT_MAX)
            return;                     /* would not fit; leave it alone */
        memmove(x->text + pos + to_len, x->text + pos + from_len,
                (size_t)tail + 1);      /* +1 carries the terminator */
        memcpy(x->text + pos, to, (size_t)to_len);
        x->text_len += to_len - from_len;

        at = pos + to_len;
    }
}

/* Whole-word rewrites applied before the rules run: abbreviations expanded,
 * then the respellings for words the rules get wrong.  Order matters -- " DR "
 * becomes " DOCTOR " first, so the expansion is then subject to the ordinary
 * rules. */
static void apply_exceptions(kl_text_ctx *x)
{
    size_t i;
    for (i = 0; i < KL_LTS_ABBREVIATION_COUNT; i++)
        replace_all(x, KL_LTS_ABBREVIATIONS[i][0], KL_LTS_ABBREVIATIONS[i][1]);
    for (i = 0; i < KL_LTS_EXCEPTION_COUNT; i++)
        replace_all(x, KL_LTS_EXCEPTIONS[i][0], KL_LTS_EXCEPTIONS[i][1]);
}

/* Load the working buffer: a leading space, the upper-cased text, a trailing
 * space.  The padding is what lets the context matchers walk one character
 * past the ends without a bounds test. */
static void load_text(kl_text_ctx *x, const char *text, int len)
{
    int i;
    int room = KL_TEXT_MAX - 3;
    if (len > room)
        len = room;
    x->text[0] = ' ';
    for (i = 0; i < len; i++)
        x->text[1 + i] = to_upper(text[i]);
    x->text[1 + len] = ' ';
    x->text[2 + len] = '\0';
    x->text_len = len + 2;
}

/* Pass 1: English spelling to ARPABET.  Upper-case letters spell out
 * two-letter ARPABET symbols; lower-case letters are single-letter consonants.
 * That convention is the tables', and pass 2 relies on it. */
static void to_arpabet(kl_text_ctx *x, const char *text, int len)
{
    int pos = 1, n;

    load_text(x, text, len);
    apply_exceptions(x);
    n = x->text_len;

    x->arpa_len = 0;
    x->arpa_full = 0;

    while (pos < n - 1) {
        char c = x->text[pos];
        const kl_lts_rule_group *group;
        size_t i;
        int matched = 0;

        x->cur_src = pos;
        if (is_digit(c)) {
            pos = read_number(x, pos, n);
            continue;
        }
        if (c == '$' && pos + 1 < n && is_digit(x->text[pos + 1])) {
            pos = read_money(x, pos, n);
            continue;
        }

        group = is_upper(c) ? &KL_LTS_NRL_RULES[1 + (c - 'A')]
                            : &KL_LTS_NRL_RULES[0];

        for (i = 0; i < group->count; i++) {
            const kl_lts_rule *rule = &group->rules[i];
            int rlen = (int)strlen(rule->match);
            if (strncmp(x->text + pos, rule->match, (size_t)rlen) != 0)
                continue;
            if (!match_left(rule->left, x->text, pos - 1))
                continue;
            if (!match_right(rule->right, x->text, n, pos + rlen))
                continue;
            arpa_puts(x, rule->out);
            pos += rlen;
            matched = 1;
            break;
        }
        if (!matched)
            pos++;   /* no rule: drop the character rather than stall */
    }
    x->arpa[x->arpa_len] = '\0';
}

/* ---------------------------------------------------------------- pass 2 --
 *
 * The rules' ARPABET onto the klatt1980-en bank.  The rules write upper-case
 * pairs for two-letter symbols and lower-case letters for single consonants,
 * with two spellings the bank does not have:
 *
 *   AX  schwa.  The bank has no reduced vowel -- Klatt 1980's table has none
 *       either -- so it becomes AH, the vowel CMU's dictionary writes schwa
 *       as (AH0).  It is flagged as reduced, which pass 3 reads: a schwa is
 *       never the stressed syllable.
 *   WH  "where", "what".  Most English speakers merged it with W long ago,
 *       and the bank has no voiceless /w/.
 *
 * And h, j and y are the rules' spellings of HH, JH and Y. */

enum {
    KL_TEXT_ITEM_PHONE = 0,
    KL_TEXT_ITEM_BREAK,   /* between words */
    KL_TEXT_ITEM_PAUSE    /* `,` `;` or `.` */
};

#define F_VOWEL    1
#define F_REDUCED  2
#define F_STRESS   4
#define F_STICKY   8      /* delta10 is a sticky pitch change   */
#define F_TRANS   16      /* delta10 is a transient pitch move  */
#define F_DECLINE 32      /* a `b-` declination step goes first */

static const char *const PAIRS[] = {
    "AA", "AE", "AH", "AO", "AW", "AX", "AY", "CH", "DH", "EH", "ER", "EY",
    "IH", "IY", "NG", "OW", "OY", "SH", "TH", "UH", "UW", "WH", "ZH",
};

static int is_pair(char a, char b)
{
    size_t i;
    for (i = 0; i < sizeof PAIRS / sizeof PAIRS[0]; i++)
        if (PAIRS[i][0] == a && PAIRS[i][1] == b)
            return 1;
    return 0;
}

static int is_vowel_symbol(const char *s)
{
    static const char *const V[] = {
        "AA", "AE", "AH", "AO", "AW", "AY", "EH", "ER", "EY", "IH", "IY",
        "OW", "OY", "UH", "UW",
    };
    size_t i;
    for (i = 0; i < sizeof V / sizeof V[0]; i++)
        if (strcmp(V[i], s) == 0)
            return 1;
    return 0;
}

static kl_text_item *add_item(kl_text_ctx *x, int kind, const char *sym,
                              int src)
{
    kl_text_item *it;
    if (x->item_count >= KL_TEXT_ITEMS)
        return NULL;
    it = &x->items[x->item_count++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    it->src = src;
    if (sym) {
        size_t n = strlen(sym);
        if (n > 3) n = 3;
        memcpy(it->sym, sym, n);
        it->sym[n] = '\0';
    }
    return it;
}

static void symbolize(kl_text_ctx *x)
{
    int i = 0;
    x->item_count = 0;

    while (i < x->arpa_len) {
        char c = x->arpa[i];
        int src = x->arpa_src[i];
        char sym[3];
        kl_text_item *it;

        if (c == ' ') {
            if (x->item_count == 0 ||
                x->items[x->item_count - 1].kind == KL_TEXT_ITEM_PHONE)
                add_item(x, KL_TEXT_ITEM_BREAK, NULL, src);
            i++;
            continue;
        }
        if (c == ',' || c == ';' || c == '.') {
            sym[0] = c;
            sym[1] = '\0';
            add_item(x, KL_TEXT_ITEM_PAUSE, sym, src);
            i++;
            continue;
        }
        if (is_upper(c) && i + 1 < x->arpa_len && is_pair(c, x->arpa[i + 1])) {
            int reduced = 0;
            sym[0] = c;
            sym[1] = x->arpa[i + 1];
            sym[2] = '\0';
            i += 2;
            if (strcmp(sym, "AX") == 0) {
                memcpy(sym, "AH", 3);
                reduced = 1;
            } else if (strcmp(sym, "WH") == 0) {
                memcpy(sym, "W", 2);
            }
            it = add_item(x, KL_TEXT_ITEM_PHONE, sym, src);
            if (it && is_vowel_symbol(sym))
                it->flags |= F_VOWEL | (reduced ? F_REDUCED : 0);
            continue;
        }
        i++;
        switch (c) {
        case 'h': add_item(x, KL_TEXT_ITEM_PHONE, "HH", src); break;
        case 'j': add_item(x, KL_TEXT_ITEM_PHONE, "JH", src); break;
        case 'b': case 'd': case 'f': case 'g': case 'k': case 'l': case 'm':
        case 'n': case 'p': case 'r': case 's': case 't': case 'v': case 'w':
        case 'y': case 'z':
            sym[0] = to_upper(c);
            sym[1] = '\0';
            add_item(x, KL_TEXT_ITEM_PHONE, sym, src);
            break;
        default:
            break;   /* nothing else is in the tables' alphabet */
        }
    }
}

/* ---------------------------------------------------------------- pass 3 --
 *
 * Stress.  The NRL rules give none, and klattsch without it gives every
 * syllable the same length and pitch -- which is what makes a rule synth sound
 * like it is reading a list.  One primary stress per content word, placed
 * from spelling:
 *
 *   - function words take none (they are unstressed in running speech);
 *   - one syllable takes it;
 *   - otherwise, after stripping endings that never move stress (-s, -ed,
 *     -ing, -ly, -ness, -ment, -ful, -less), the ending decides where one is
 *     known to (see the three tables below), then a short prefix list for
 *     two-syllable words, then the first syllable -- English's default;
 *   - a schwa never takes it; the next full vowel does.
 *
 * The tables and the order were settled by measurement against the CMU
 * dictionary, not by taste; docs/19-frontend-text.md has the numbers for each
 * alternative that was tried and dropped. */

static const char *const FUNCTION_WORDS[] = {
    "A", "AN", "THE", "AND", "OR", "BUT", "NOR", "OF", "TO", "IN", "ON", "AT",
    "BY", "FOR", "FROM", "WITH", "AS", "IS", "AM", "ARE", "WAS", "WERE", "BE",
    "BEEN", "HAS", "HAVE", "HAD", "DO", "DOES", "DID", "CAN", "COULD", "WILL",
    "WOULD", "SHALL", "SHOULD", "MAY", "MIGHT", "MUST", "I", "ME", "MY", "WE",
    "US", "OUR", "YOU", "YOUR", "HE", "HIM", "HIS", "SHE", "HER", "IT", "ITS",
    "THEY", "THEM", "THEIR", "THAT", "THAN", "IF", "SO", "INTO", "ONTO", "UPON",
    "WHICH", "WHO", "WHOM", "WHOSE", "THERE", "IT'S", "I'M", "YOU'RE",
    "WE'RE", "THEY'RE", "I'VE", "I'LL", "I'D",
};

/* Endings that carry the stress on their own first syllable:
 * refugEE, voluntEER, JapanESE, cigarETTE, balLOON, uNIQUE, biOLogy. */
static const char *const SELF_STRESSED[] = {
    "OLOGY", "OLOGIST", "OLOG", "OGRAPHY", "OMETER", "ESQUE", "IQUE", "ETTE", "EER",
    "ESE", "OON", "EEN", "EE",
};

/* Endings that put the stress on the syllable just before them:
 * naTION, muSIcian, eLECtric, abILity, deLIcious, faMILiar. */
static const char *const PRE_STRESSING[] = {
    "TIONAL", "SIONAL", "TION", "SION", "CION", "TIAL", "CIAL", "TIAN",
    "CIAN", "SIAN", "TIOUS", "CIOUS", "GIOUS", "EOUS", "IOUS", "UOUS", "IAL",
    "IAN", "UAL", "ICAL", "ICS", "IC", "ITY", "ETY", "IFY", "ITIVE", "ULAR",
    "IUM", "IOR", "IENT", "IENCE", "IA", "ATIVE",
};

/* Endings that put it two syllables before them:
 * EDucate, ORganize, NECessary, TERritory. */
static const char *const TWO_BEFORE[] = {
    "ATE", "IZE", "ISE", "ARY", "ORY",
};

/* Two-syllable words beginning like this are usually stressed on the second:
 * beGIN, deCIDE, reTURN, conSIST, exPLAIN. */
static const char *const PREFIXES[] = {
    "BE", "DE", "RE", "CON", "COM", "EX", "DIS", "MIS", "OB", "SUB", "AD",
    "EN", "EM", "PRE", "PER", "AB", "SUR", "IN", "IM", "IL", "IR", "UN",
    /* a- before a doubled consonant: aCCEPT, aFFECT, aTTACH, aPPLAUD */
    "ACC", "ACQ", "AFF", "AGG", "ALL", "ANN", "APP", "ASS", "ATT",
};

/* Endings that never move the stress, stripped before the tables look. */
static const char *const NEUTRAL[] = {
    "NESS", "MENT", "LESS", "SHIP", "FUL", "ABLE", "IBLE", "ING", "IST",
    "ISM", "ED", "ER", "LY", "S",
};

#define COUNT(a) (sizeof (a) / sizeof (a)[0])

static int word_is(const char *w, int wl, const char *const *list, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if ((int)strlen(list[i]) == wl && memcmp(list[i], w, (size_t)wl) == 0)
            return 1;
    return 0;
}

static int ends_with(const char *w, int wl, const char *suf)
{
    int sl = (int)strlen(suf);
    return sl <= wl && memcmp(w + wl - sl, suf, (size_t)sl) == 0;
}

static int has_vowel_letter(const char *w, int wl)
{
    int i;
    for (i = 0; i < wl; i++)
        if (is_vowel(w[i]) || w[i] == 'Y')
            return 1;
    return 0;
}

/* The word's vowels as item indices; returns how many. */
static int word_vowels(const kl_text_ctx *x, int a, int b, int *v, int cap)
{
    int i, n = 0;
    for (i = a; i < b; i++)
        if ((x->items[i].flags & F_VOWEL) && n < cap)
            v[n++] = i;
    return n;
}

/* Of the word's vowels, the last whose rule started before text index `at`,
 * `back` places further back (0 = the last); -1 if there is none. */
static int vowel_before(const kl_text_ctx *x, const int *v, int nv, int at,
                        int back)
{
    int i;
    for (i = nv - 1; i >= 0; i--) {
        if (x->items[v[i]].src < at) {
            if (back == 0)
                return v[i];
            back--;
        }
    }
    return -1;
}

static int vowel_from(const kl_text_ctx *x, const int *v, int nv, int at)
{
    int i;
    for (i = 0; i < nv; i++)
        if (x->items[v[i]].src >= at)
            return v[i];
    return -1;
}

#define MAX_WORD_VOWELS 64

/* Do items [a, b) spell exactly `syms`, space-separated? */
static int word_spells(const kl_text_ctx *x, int a, int b, const char *syms)
{
    int i;
    for (i = a; i < b; i++) {
        size_t n = strlen(x->items[i].sym);
        if (strncmp(syms, x->items[i].sym, n) != 0)
            return 0;
        syms += n;
        if (*syms == ' ')
            syms++;
        else if (i + 1 < b)
            return 0;
    }
    return *syms == '\0';
}

/* A word the number reader wrote has no spelling to read stress from, and
 * does not need one: its words are a closed set.  "and" is unstressed
 * ("three thousand and five"), the -teens take it on the teen (the only
 * number words that do), and every other number word on its first vowel. */
static int number_stress(const kl_text_ctx *x, int a, int b, const int *v,
                         int nv)
{
    if (word_spells(x, a, b, "AA N D"))
        return -1;
    if (b - a >= 3 && strcmp(x->items[b - 3].sym, "T") == 0 &&
        strcmp(x->items[b - 2].sym, "IY") == 0 &&
        strcmp(x->items[b - 1].sym, "N") == 0)
        return v[nv - 1];
    return v[0];
}

/* Where the stress goes in items [a, b), a word whose spelling is `w` (upper
 * case, `wl` letters) starting at working-text index `ws`.  -1 for none. */
static int choose_stress(const kl_text_ctx *x, int a, int b,
                         const char *w, int wl, int ws)
{
    int v[MAX_WORD_VOWELS];
    int nv = word_vowels(x, a, b, v, MAX_WORD_VOWELS);
    char stem[64];
    int sl, pass;
    size_t i;

    if (nv == 0)
        return -1;
    if (!w)
        return number_stress(x, a, b, v, nv);
    if (word_is(w, wl, FUNCTION_WORDS, COUNT(FUNCTION_WORDS)))
        return -1;
    if (nv == 1)
        return v[0];
    if (wl >= (int)sizeof stem)
        return v[0];

    /* The stem: the word with its neutral endings taken off, and -IES/-IED
     * written back as -Y so that "abilities" is seen as "ability".  The copy
     * and the working text agree up to the point where an ending was changed,
     * which is always after the suffix the tables look for starts -- so a
     * suffix's position in the copy is its position in the text. */
    memcpy(stem, w, (size_t)wl);
    sl = wl;
    if (ends_with(stem, sl, "'S"))
        sl -= 2;
    for (pass = 0; pass < 3; pass++) {
        int stripped = 0;
        if (sl > 4 && (ends_with(stem, sl, "IES") || ends_with(stem, sl, "IED"))) {
            sl -= 2;
            stem[sl - 1] = 'Y';
            continue;
        }
        for (i = 0; i < COUNT(NEUTRAL); i++) {
            int n = (int)strlen(NEUTRAL[i]);
            if (!ends_with(stem, sl, NEUTRAL[i]))
                continue;
            if (sl - n < 3 || !has_vowel_letter(stem, sl - n))
                continue;
            if (n == 1 && (ends_with(stem, sl, "SS") || ends_with(stem, sl, "US") ||
                           ends_with(stem, sl, "IS")))
                continue;
            sl -= n;
            stripped = 1;
            break;
        }
        if (!stripped)
            break;
    }

    for (i = 0; i < COUNT(SELF_STRESSED); i++) {
        int n = (int)strlen(SELF_STRESSED[i]);
        if (sl > n + 1 && ends_with(stem, sl, SELF_STRESSED[i])) {
            int t = vowel_from(x, v, nv, ws + sl - n);
            if (t >= 0 && vowel_before(x, v, nv, ws + sl - n, 0) >= 0)
                return t;
        }
    }
    for (i = 0; i < COUNT(PRE_STRESSING); i++) {
        int n = (int)strlen(PRE_STRESSING[i]);
        if (sl > n && ends_with(stem, sl, PRE_STRESSING[i])) {
            int t = vowel_before(x, v, nv, ws + sl - n, 0);
            if (t >= 0)
                return t;
        }
    }
    for (i = 0; i < COUNT(TWO_BEFORE); i++) {
        int n = (int)strlen(TWO_BEFORE[i]);
        if (sl > n && ends_with(stem, sl, TWO_BEFORE[i])) {
            int t = vowel_before(x, v, nv, ws + sl - n, 1);
            if (t >= 0)
                return t;
        }
    }
    {
        int stem_vowels = 0;
        for (i = 0; i < (size_t)nv; i++)
            if (x->items[v[i]].src < ws + sl)
                stem_vowels++;
        if (stem_vowels >= 2) {
            for (i = 0; i < COUNT(PREFIXES); i++) {
                int n = (int)strlen(PREFIXES[i]);
                if (sl > n + 2 && memcmp(stem, PREFIXES[i], (size_t)n) == 0 &&
                    x->items[v[0]].src < ws + n && x->items[v[1]].src >= ws + n)
                    return v[1];
            }
        }
        if (stem_vowels >= 3)
            return v[stem_vowels - 3];
    }
    return v[0];
}

/* A schwa is never the stressed syllable: move to the next full vowel, or
 * failing that the previous one. */
static int unreduce(const kl_text_ctx *x, int a, int b, int t)
{
    int i;
    if (t < 0 || !(x->items[t].flags & F_REDUCED))
        return t;
    for (i = t + 1; i < b; i++)
        if ((x->items[i].flags & F_VOWEL) && !(x->items[i].flags & F_REDUCED))
            return i;
    for (i = t - 1; i >= a; i--)
        if ((x->items[i].flags & F_VOWEL) && !(x->items[i].flags & F_REDUCED))
            return i;
    return t;
}

/* The word in the working text that produced the phoneme at `src`: letters
 * and apostrophes either side of it.  Returns its length, 0 for a number (a
 * digit run is read by pass 1's number reader, and has no spelling to read
 * stress from). */
static int word_at(const kl_text_ctx *x, int src, int *start)
{
    int s = src, e = src;
    if (src < 0 || src >= x->text_len || is_digit(x->text[src]) ||
        x->text[src] == '$')
        return 0;
    while (s > 0 && (is_upper(x->text[s - 1]) || x->text[s - 1] == '\''))
        s--;
    while (e < x->text_len && (is_upper(x->text[e]) || x->text[e] == '\''))
        e++;
    *start = s;
    return e - s;
}

static void assign_stress(kl_text_ctx *x)
{
    int a = 0;
    while (a < x->item_count) {
        int b = a, ws = 0, wl, t;
        if (x->items[a].kind != KL_TEXT_ITEM_PHONE) {
            a++;
            continue;
        }
        while (b < x->item_count && x->items[b].kind == KL_TEXT_ITEM_PHONE)
            b++;
        wl = word_at(x, x->items[a].src, &ws);
        t = choose_stress(x, a, b, wl ? x->text + ws : NULL, wl, ws);
        t = unreduce(x, a, b, t);
        if (t >= 0)
            x->items[t].flags |= F_STRESS;
        a = b;
    }
}

/* ---------------------------------------------------------------- pass 4 --
 *
 * The sentence contour, written as klattsch source.  Each sentence starts
 * with a bare `b`, which resets F0 to the compiler's base -- so the contour
 * never drifts from one sentence to the next, and never overrides the pitch
 * the caller chose.  Then, as fractions of the base F0:
 *
 *                    start   decline over    nucleus         final vowel
 *                            the sentence
 *   statement  .     +8%     -10%            fall -20%        --
 *   exclamation !   +15%     -12%            fall -25%        --
 *   question   ?     +4%      -4%            --               rise +30%
 *
 * The nucleus is the last stressed syllable.  Its fall is a sticky pitch
 * delta, so everything after it stays low -- "I SAW it" does not bounce back
 * up on "it".  The decline is spread in equal `b-` steps before each stressed
 * syllable after the first.  A comma or semicolon inside a sentence gets a
 * small transient rise (+8%) on the vowel before it: the continuation rise
 * that tells a listener the sentence is not over.
 *
 * These are the first numbers that sound like English, not measured ones --
 * docs/19-frontend-text.md says so, and says what was tried.  They are all in
 * this table and nowhere else. */

typedef enum { S_STATEMENT, S_QUESTION, S_EXCLAMATION } sentence_type;

static const struct {
    double start, decline, nucleus, final_rise;
} CONTOUR[] = {
    /* statement   */ { 0.08, 0.10, -0.20, 0.0 },
    /* question    */ { 0.04, 0.04,  0.0,  0.30 },
    /* exclamation */ { 0.15, 0.12, -0.25, 0.0 },
};
#define CONTINUATION_RISE 0.08

/* Tenths of a hertz, rounded half away from zero, without libm. */
static int tenths(double hz)
{
    double t = hz * 10.0;
    return (int)(t < 0 ? t - 0.5 : t + 0.5);
}

static void apply_contour(kl_text_ctx *x, sentence_type type, double base,
                          int *start10, int *decline10)
{
    int i, first_stress = -1, nucleus = -1, last_vowel = -1, stresses = 0;
    int clause_last_vowel = -1;

    for (i = 0; i < x->item_count; i++) {
        kl_text_item *it = &x->items[i];
        if (it->kind == KL_TEXT_ITEM_PHONE && (it->flags & F_VOWEL))
            last_vowel = i;
        if (it->flags & F_STRESS) {
            if (first_stress < 0)
                first_stress = i;
            nucleus = i;
            stresses++;
        }
    }
    *start10 = 0;
    *decline10 = 0;
    if (last_vowel < 0)
        return;
    if (nucleus < 0)
        nucleus = last_vowel;

    *start10 = tenths(CONTOUR[type].start * base);
    if (stresses > 1) {
        *decline10 = tenths(CONTOUR[type].decline * base / (stresses - 1));
        for (i = 0; i < x->item_count; i++)
            if ((x->items[i].flags & F_STRESS) && i != first_stress &&
                *decline10 != 0)
                x->items[i].flags |= F_DECLINE;
    }

    /* Continuation rises: the last vowel before each mid-sentence , or ; */
    for (i = 0; i < x->item_count; i++) {
        kl_text_item *it = &x->items[i];
        if (it->kind == KL_TEXT_ITEM_PHONE && (it->flags & F_VOWEL)) {
            clause_last_vowel = i;
        } else if (it->kind == KL_TEXT_ITEM_PAUSE && it->sym[0] != '.' &&
                   clause_last_vowel >= 0 && clause_last_vowel < nucleus) {
            x->items[clause_last_vowel].flags |= F_TRANS;
            x->items[clause_last_vowel].delta10 =
                tenths(CONTINUATION_RISE * base);
            clause_last_vowel = -1;
        }
    }

    if (CONTOUR[type].nucleus != 0.0) {
        x->items[nucleus].flags &= ~F_TRANS;
        x->items[nucleus].flags |= F_STICKY;
        x->items[nucleus].delta10 = tenths(CONTOUR[type].nucleus * base);
    }
    if (CONTOUR[type].final_rise != 0.0) {
        x->items[last_vowel].flags &= ~F_TRANS;
        x->items[last_vowel].flags |= F_STICKY;
        x->items[last_vowel].delta10 = tenths(CONTOUR[type].final_rise * base);
    }
}

/* ----------------------------------------------------------------- output --
 *
 * Tokens separated by single spaces.  Once one token does not fit, nothing
 * more is written -- a later, shorter token would otherwise land after a gap
 * -- but the length keeps counting, so the caller learns the full size. */

typedef struct {
    char  *out;
    size_t cap;
    size_t len;      /* full length, as if unbounded */
    size_t written;  /* bytes actually in out        */
    int    full;
} sink;

static void sink_token(sink *s, const char *tok)
{
    size_t n = strlen(tok);
    size_t need = n + (s->len ? 1 : 0);
    if (!s->full && s->out && s->written + need < s->cap) {
        if (s->len)
            s->out[s->written++] = ' ';
        memcpy(s->out + s->written, tok, n);
        s->written += n;
    } else {
        s->full = 1;
    }
    s->len += need;
}

static void sink_finish(sink *s)
{
    if (s->out && s->cap > 0)
        s->out[s->written < s->cap ? s->written : s->cap - 1] = '\0';
}

/* "+9.6", "-24", "+0.5" from tenths of a hertz. */
static void format_delta(char *buf, int t10)
{
    char digits[16];
    int n = 0, whole, frac;
    unsigned int u = (unsigned int)(t10 < 0 ? -t10 : t10);
    *buf++ = t10 < 0 ? '-' : '+';
    whole = (int)(u / 10u);
    frac = (int)(u % 10u);
    do {
        digits[n++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole > 0);
    while (n > 0)
        *buf++ = digits[--n];
    if (frac) {
        *buf++ = '.';
        *buf++ = (char)('0' + frac);
    }
    *buf = '\0';
}

/* A comma is written as `p<ms>` when `comma_ms` is set, and as the engine's
 * own 100 ms `,` token when it is 0 (spelling, where a comma separates
 * characters and the short pause is the point). */
static void emit_items(const kl_text_ctx *x, sink *s, int with_pauses,
                       int decline10, int comma_ms)
{
    int i;
    char tok[40], d[16];
    for (i = 0; i < x->item_count; i++) {
        const kl_text_item *it = &x->items[i];
        if (it->kind == KL_TEXT_ITEM_PAUSE) {
            if (!with_pauses)
                continue;
            if (it->sym[0] == ',' && comma_ms > 0) {
                format_delta(d, comma_ms * 10);
                tok[0] = 'p';
                strcpy(tok + 1, d + 1);   /* "p200": the value, no sign */
                sink_token(s, tok);
            } else {
                sink_token(s, it->sym);
            }
            continue;
        }
        if (it->kind != KL_TEXT_ITEM_PHONE)
            continue;
        if (it->flags & F_DECLINE) {
            format_delta(d, -decline10);
            tok[0] = 'b';
            strcpy(tok + 1, d);
            sink_token(s, tok);
        }
        strcpy(tok, it->sym);
        if (it->flags & F_STRESS)
            strcat(tok, "'");
        if ((it->flags & (F_STICKY | F_TRANS)) && it->delta10 != 0) {
            format_delta(d, it->delta10);
            if (it->flags & F_TRANS) {
                strcat(tok, "(");
                strcat(tok, d);
                strcat(tok, ")");
            } else {
                strcat(tok, d);
            }
        }
        sink_token(s, tok);
    }
}

/* ------------------------------------------------------ text preparation --
 *
 * NVDA hands over UTF-8 with typographic punctuation in it, and the rules
 * read ASCII.  Before a sentence reaches them, each code point is folded:
 * curly quotes to straight, an ellipsis to a full stop, an em dash to a
 * comma (it is a clause break when spoken), the no-break and thin spaces to
 * a space, and the accented letters of Latin-1 to their base letters --
 * "café" is read as "cafe", which is how an English reader says it.
 * Anything else outside ASCII becomes a space, so it ends a word rather than
 * gluing two together. */

static int utf8_next(const unsigned char **pp, const unsigned char *end)
{
    const unsigned char *p = *pp;
    unsigned int c = p[0], cp;
    int n, i;
    if (c < 0x80) { *pp = p + 1; return (int)c; }
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { *pp = p + 1; return 0xFFFD; }
    if (end - p <= n) { *pp = end; return 0xFFFD; }
    for (i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp = p + i; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    *pp = p + n + 1;
    return (int)cp;
}

/* Latin-1 Supplement letters U+00C0..U+00FF to ASCII, upper and lower case
 * folded the same way ("" where a letter has no plain reading). */
static const char *const LATIN1[64] = {
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O", " ", "O", "U", "U", "U", "U", "Y", "TH", "SS",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", " ", "o", "u", "u", "u", "u", "y", "th", "y",
};

static const char *fold(int cp)
{
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201B: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2026: return ".";
    case 0x2014: return ",";
    case 0x2013: case 0x2010: case 0x2011: return "-";
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F:
        return " ";
    default: break;
    }
    if (cp >= 0xC0 && cp <= 0xFF)
        return LATIN1[cp - 0xC0];
    return " ";
}

/* ----------------------------------------------------------- abbreviations --
 *
 * Abbreviations written with a full stop.  Two things go wrong with them if
 * nothing is done: the full stop ends the sentence (so "Dr. Smith" is two
 * sentences, the first a lone falling "doctor"), and the rules spell them out
 * or read them as a word ("D R", "E G").  Votraxxion's three abbreviations
 * in kl_text_rules.c only match without the stop.
 *
 * So these are expanded before the rules see the sentence, and the splitter
 * below does not end a sentence on their stop.  "etc." is the exception to
 * the second half: it usually does end one, and keeps its stop.
 *
 * Deliberately short.  "St." (saint or street) and "No." (number or no) are
 * left out because the wrong guess is worse than the spelled-out letters, and
 * symbol names (& % @ ...) are left to NVDA, whose symbol processing replaces
 * them according to the user's punctuation level before the text arrives. */

static const char *const ABBREVIATIONS[][2] = {
    { "MR.",   "MISTER" },
    { "MRS.",  "MISSUS" },
    { "MS.",   "MIZ" },
    { "DR.",   "DOCTOR" },
    { "PROF.", "PROFESSOR" },
    { "JR.",   "JUNIOR" },
    { "SR.",   "SENIOR" },
    { "VS.",   "VERSUS" },
    { "E.G.",  "FOR EXAMPLE" },
    { "I.E.",  "THAT IS" },
    { "ETC.",  "ET CETERA." },
};

/* Case-insensitive: does `abbr` (upper case, with its stop) stand at `t` as a
 * whole word?  `t` is `len` bytes; `at` is where to look. */
static int abbreviation_at(const char *t, int len, int at, const char *abbr)
{
    int k, n = (int)strlen(abbr);
    if (at > 0 && is_alnum(t[at - 1]))
        return 0;
    if (at + n > len)
        return 0;
    for (k = 0; k < n; k++)
        if (to_upper(t[at + k]) != abbr[k])
            return 0;
    return at + n == len || !is_alnum(t[at + n]);
}

/* Is the stop at t[n - 1] the end of an abbreviation that does not end a
 * sentence? */
static int abbreviation_ends_at(const char *t, int n)
{
    size_t i;
    for (i = 0; i < COUNT(ABBREVIATIONS); i++) {
        int k = n - (int)strlen(ABBREVIATIONS[i][0]);
        if (strcmp(ABBREVIATIONS[i][0], "ETC.") == 0)
            continue;
        if (k >= 0 && abbreviation_at(t, n, k, ABBREVIATIONS[i][0]))
            return 1;
    }
    return 0;
}

/* The sentence with its abbreviations written out, into x->expanded.
 * Returns the new length; text that would overflow is cut. */
static int expand_abbreviations(kl_text_ctx *x, const char *t, int len)
{
    int i = 0, o = 0;
    while (i < len && o < KL_TEXT_MAX - 1) {
        size_t a;
        int hit = 0;
        for (a = 0; a < COUNT(ABBREVIATIONS); a++) {
            if (abbreviation_at(t, len, i, ABBREVIATIONS[a][0])) {
                const char *e = ABBREVIATIONS[a][1];
                while (*e && o < KL_TEXT_MAX - 1)
                    x->expanded[o++] = *e++;
                i += (int)strlen(ABBREVIATIONS[a][0]);
                hit = 1;
                break;
            }
        }
        if (!hit)
            x->expanded[o++] = t[i++];
    }
    return o;
}

/* --------------------------------------------------------------- sentences --
 *
 * Split at . ? or ! followed by whitespace or the end -- the trailing
 * whitespace requirement is what keeps "3.14" in one piece -- unless the stop
 * belongs to one of the abbreviations above. */

static sentence_type classify(char terminator)
{
    if (terminator == '?') return S_QUESTION;
    if (terminator == '!') return S_EXCLAMATION;
    return S_STATEMENT;
}

static void speak_sentence(kl_text_ctx *x, const char *text, int len,
                           double base, int comma_ms, sink *s)
{
    int j, start10, decline10;
    char terminator = ' ', tok[24];

    to_arpabet(x, x->expanded, expand_abbreviations(x, text, len));
    symbolize(x);
    assign_stress(x);

    for (j = len - 1; j >= 0; j--) {
        if (!is_space(text[j])) {
            terminator = text[j];
            break;
        }
    }
    apply_contour(x, classify(terminator), base, &start10, &decline10);

    for (j = 0; j < x->item_count; j++)
        if (x->items[j].flags & F_VOWEL)
            break;
    if (j < x->item_count) {
        sink_token(s, "b");
        if (start10 != 0) {
            tok[0] = 'b';
            format_delta(tok + 1, start10);
            sink_token(s, tok);
        }
    }
    emit_items(x, s, 1, decline10, comma_ms);
}

/* Folding happens code point by code point into x->sent, and a sentence is
 * handed on when its terminator is followed by whitespace, or when it has
 * filled the buffer (then it is simply cut there -- see KL_TEXT_MAX). */
size_t kl_text_to_source(kl_text_ctx *x, const char *text,
                         const kl_text_opts *opts, char *out, size_t cap)
{
    static const size_t room = KL_TEXT_MAX - 8;
    char *sent;
    size_t n = 0;
    double base = (opts && opts->base_f0 > 0.0) ? opts->base_f0 : 120.0;
    int comma_ms = (opts && opts->comma_ms > 0) ? opts->comma_ms : KL_TEXT_COMMA_MS;
    const unsigned char *p, *end;
    sink s;

    memset(&s, 0, sizeof s);
    s.out = out;
    s.cap = cap;
    if (!x || !text) {
        sink_finish(&s);
        return 0;
    }
    sent = x->sent;
    p = (const unsigned char *)text;
    end = p + strlen(text);

    while (p < end) {
        int cp = utf8_next(&p, end);
        const char *f;
        char buf[2];
        int boundary = 0;

        if (cp < 0x80) {
            buf[0] = (char)cp;
            buf[1] = '\0';
            f = buf;
        } else {
            f = fold(cp);
        }
        for (; *f && n < room; f++)
            sent[n++] = *f;

        if (n > 0 && (sent[n - 1] == '.' || sent[n - 1] == '?' ||
                      sent[n - 1] == '!')) {
            if ((p >= end || is_space((char)*p)) &&
                !abbreviation_ends_at(sent, (int)n))
                boundary = 1;
        }
        if (boundary || n >= room) {
            speak_sentence(x, sent, (int)n, base, comma_ms, &s);
            n = 0;
        }
    }
    if (n > 0)
        speak_sentence(x, sent, (int)n, base, comma_ms, &s);

    sink_finish(&s);
    return s.len;
}

size_t kl_text_word(kl_text_ctx *x, const char *word, char *out, size_t cap)
{
    sink s;
    memset(&s, 0, sizeof s);
    s.out = out;
    s.cap = cap;
    if (x && word) {
        to_arpabet(x, word, (int)strlen(word));
        symbolize(x);
        assign_stress(x);
        emit_items(x, &s, 0, 0, 0);
    }
    sink_finish(&s);
    return s.len;
}

size_t kl_text_nrl(kl_text_ctx *x, const char *text, char *out, size_t cap)
{
    size_t n;
    if (!x || !text) {
        if (out && cap) out[0] = '\0';
        return 0;
    }
    to_arpabet(x, text, (int)strlen(text));
    n = (size_t)x->arpa_len;
    if (out && cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(out, x->arpa, c);
        out[c] = '\0';
    }
    return n;
}

/* Spelling: every character by its name from the ASCII table, stressed on its
 * first full vowel (the names have no spelling of their own to read stress
 * from, and a letter name is stressed there: "DOUble-u", "QUEStion mark"),
 * a comma between characters, and no contour -- a spelled string is a list,
 * not a sentence. */
size_t kl_text_spell(kl_text_ctx *x, const char *text, char *out, size_t cap)
{
    const unsigned char *p;
    sink s;
    int first = 1;

    memset(&s, 0, sizeof s);
    s.out = out;
    s.cap = cap;
    if (!x || !text) {
        sink_finish(&s);
        return 0;
    }
    x->arpa_len = 0;
    x->arpa_full = 0;
    x->cur_src = -1;
    for (p = (const unsigned char *)text; *p; p++) {
        if (*p > 127)
            continue;
        if (!first)
            arpa_putc(x, ',');
        arpa_putc(x, ' ');
        arpa_puts(x, KL_LTS_ASCII_NAMES[*p]);
        arpa_putc(x, ' ');
        first = 0;
    }
    x->arpa[x->arpa_len] = '\0';
    symbolize(x);
    {
        int a = 0;
        while (a < x->item_count) {
            int b = a, i;
            if (x->items[a].kind != KL_TEXT_ITEM_PHONE) {
                a++;
                continue;
            }
            while (b < x->item_count && x->items[b].kind == KL_TEXT_ITEM_PHONE)
                b++;
            for (i = a; i < b; i++)
                if ((x->items[i].flags & F_VOWEL) &&
                    !(x->items[i].flags & F_REDUCED)) {
                    x->items[i].flags |= F_STRESS;
                    break;
                }
            a = b;
        }
    }
    emit_items(x, &s, 1, 0, 0);
    sink_finish(&s);
    return s.len;
}
