/* kl_dict.c -- lookup in the compiled-in CMU dictionary. See kl_dict.h.
 *
 * The table (kl_dict_data.c, generated at build time) is a set of string
 * chunks, each at most 4,095 bytes -- the longest string literal C17 requires
 * a compiler to accept, so gcc's -Wpedantic stays quiet and MSVC's 64 KB cap
 * is never near -- holding one entry per line, "KEY CODES\n", and an index of
 * where each entry starts: chunk << 12 | offset. Keys are sorted by byte, so
 * the lookup is a binary search; nothing is allocated and nothing is copied.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Päiv Dengo (this code; the data is CMU's)
 */
#include <stdint.h>
#include <string.h>

#include "kl_dict.h"

/* kl_dict_data.c */
extern const char *const kl_dict_chunks[];
extern const uint32_t kl_dict_index[];
extern const size_t kl_dict_entries;

static const char *entry_at(size_t i)
{
    uint32_t at = kl_dict_index[i];
    return kl_dict_chunks[at >> 12] + (at & 0xFFFu);
}

/* A key ends at its space. A key that is a prefix of the word sorts first,
 * as strcmp has it in the generator, because a space is below every
 * character a key can hold. */
static int compare(const char *key, const char *w, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char k = (unsigned char)key[i], c = (unsigned char)w[i];
        if (k == ' ')
            return -1;
        if (k != c)
            return k < c ? -1 : 1;
    }
    return key[n] == ' ' ? 0 : 1;
}

static const char *codes_of(const char *e, size_t key_len, size_t *n)
{
    const char *codes = e + key_len + 1;
    const char *end = strchr(codes, '\n');
    *n = (size_t)(end - codes);
    return codes;
}

const char *kl_dict_find(const char *key, size_t len, size_t *n)
{
    size_t lo = 0, hi = kl_dict_entries;
    if (!key || len == 0)
        return NULL;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *e = entry_at(mid);
        int c = compare(e, key, len);
        if (c == 0)
            return codes_of(e, len, n);
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

size_t kl_dict_count(void)
{
    return kl_dict_entries;
}

void kl_dict_entry(size_t i, const char **key, size_t *key_len,
                   const char **codes, size_t *n)
{
    const char *e = entry_at(i);
    size_t kl = (size_t)(strchr(e, ' ') - e);
    *key = e;
    *key_len = kl;
    *codes = codes_of(e, kl, n);
}
