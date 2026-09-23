/* Stage 2 verification: emit every compiled-in bank as a length-prefixed
 * binary stream, for tools/verify-stage2.mjs to compare field by field
 * against the JavaScript's resolved banks.
 *
 * Format, all integers little-endian:
 *
 *   u32  bank count
 *   str  default bank name
 *   per bank:
 *     str  name, display_name, language, license, source
 *     i32  schema_version
 *     u32  phoneme count
 *     per phoneme, in table order:
 *       str  code
 *       f64  voicing, F1, F2, F3, BW1, BW2, BW3, A1, A2, A3
 *       u8   is_stop
 *       u8   has_glide
 *       f64  glide F1, F2, F3        (present regardless, zero when no glide)
 *       str  ipa, example, source
 *
 *   str = u32 length then bytes, or u32 0xFFFFFFFF for NULL -- which is
 *   distinct from a present-but-empty string, because the two mean different
 *   things and a comparison that conflates them would hide a dropped field.
 *
 * The lookup functions are exercised too: see the `probe` mode.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */

#include "../kl_banks.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

static void put_u32(uint32_t v)
{
    unsigned char b[4];
    int i;
    for (i = 0; i < 4; i++) b[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
    fwrite(b, 1, 4, stdout);
}

static void put_i32(int32_t v) { put_u32((uint32_t)v); }

static void put_u8(unsigned v) { unsigned char b = (unsigned char)(v & 0xFF); fwrite(&b, 1, 1, stdout); }

static void put_f64(double v)
{
    uint64_t bits;
    unsigned char b[8];
    int i;
    memcpy(&bits, &v, sizeof bits);
    for (i = 0; i < 8; i++) b[i] = (unsigned char)((bits >> (8 * i)) & 0xFF);
    fwrite(b, 1, 8, stdout);
}

static void put_str(const char *s)
{
    if (s == NULL) { put_u32(0xFFFFFFFFu); return; }
    {
        const size_t n = strlen(s);
        put_u32((uint32_t)n);
        if (n) fwrite(s, 1, n, stdout);
    }
}

static void dump_banks(void)
{
    size_t i, j;
    put_u32((uint32_t)kl_bank_count);
    put_str(kl_default_bank);
    for (i = 0; i < kl_bank_count; i++) {
        const kl_bank *b = &kl_banks[i];
        put_str(b->name);
        put_str(b->display_name);
        put_str(b->language);
        put_str(b->license);
        put_str(b->source);
        put_i32(b->schema_version);
        put_u32((uint32_t)b->phoneme_count);
        for (j = 0; j < b->phoneme_count; j++) {
            const kl_phoneme *p = &b->phonemes[j];
            put_str(p->code);
            put_f64(p->voicing);
            put_f64(p->F1); put_f64(p->F2); put_f64(p->F3);
            put_f64(p->BW1); put_f64(p->BW2); put_f64(p->BW3);
            put_f64(p->A1); put_f64(p->A2); put_f64(p->A3);
            put_u8((unsigned)p->is_stop);
            put_u8((unsigned)p->has_glide);
            put_f64(p->glide_to.F1); put_f64(p->glide_to.F2); put_f64(p->glide_to.F3);
            put_str(p->ipa);
            put_str(p->example);
            put_str(p->source);
        }
    }
}

/* Exercise the lookup functions rather than only the data. A table that is
 * byte-perfect behind a binary search that cannot find its last entry is not
 * a working bank. */
static void dump_probe(void)
{
    size_t i, j;
    /* For every bank, look up every code it has -- proving the search finds
     * each one and returns the right row -- plus codes that must miss. */
    static const char *const misses[] = { "", "ZZZ", "aa", "A A", "~", "\x7f" };
    put_u32((uint32_t)kl_bank_count);
    for (i = 0; i < kl_bank_count; i++) {
        const kl_bank *b = kl_bank_get(kl_banks[i].name);
        put_u8(b == &kl_banks[i] ? 1u : 0u);         /* get by name is identity */
        put_u32((uint32_t)b->phoneme_count);
        for (j = 0; j < b->phoneme_count; j++) {
            const kl_phoneme *want = &b->phonemes[j];
            const kl_phoneme *got = kl_bank_find(b, want->code);
            put_u8(got == want ? 1u : 0u);
        }
        put_u32((uint32_t)(sizeof misses / sizeof misses[0]));
        for (j = 0; j < sizeof misses / sizeof misses[0]; j++) {
            put_u8(kl_bank_find(b, misses[j]) == NULL ? 1u : 0u);
        }
    }
    /* Unknown bank, NULL arguments. */
    put_u8(kl_bank_get("nope") == NULL ? 1u : 0u);
    put_u8(kl_bank_get(NULL) == NULL ? 1u : 0u);
    put_u8(kl_bank_find(NULL, "AA") == NULL ? 1u : 0u);
    put_u8(kl_bank_find(kl_bank_default(), NULL) == NULL ? 1u : 0u);
    put_u8(kl_bank_default() != NULL ? 1u : 0u);
    put_str(kl_bank_default()->name);
}

int main(int argc, char **argv)
{
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc != 2) {
        fprintf(stderr, "usage: kl_banks_dump <banks|probe>\n");
        return 2;
    }
    if      (strcmp(argv[1], "banks") == 0) dump_banks();
    else if (strcmp(argv[1], "probe") == 0) dump_probe();
    else { fprintf(stderr, "unknown section: %s\n", argv[1]); return 2; }
    return ferror(stdout) ? 1 : 0;
}
