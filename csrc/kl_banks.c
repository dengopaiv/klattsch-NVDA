/* Phoneme bank lookup.
 *
 * Translated from src/engine/banks/index.js. The registry's resolution and
 * caching are not here: the generator emits resolved tables, so there is
 * nothing to resolve at run time and nothing to cache. What remains is lookup.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */

#include "kl_banks.h"

#include <string.h>

const kl_bank *kl_bank_get(const char *name)
{
    size_t i;
    if (name == NULL) return NULL;
    /* Three banks. A linear scan over three string compares is faster than
     * anything cleverer, and this is not on the speech path -- a bank is
     * chosen per utterance at most. */
    for (i = 0; i < kl_bank_count; i++) {
        if (strcmp(kl_banks[i].name, name) == 0) return &kl_banks[i];
    }
    return NULL;
}

const kl_bank *kl_bank_default(void)
{
    return kl_bank_get(kl_default_bank);
}

const kl_phoneme *kl_bank_find(const kl_bank *bank, const char *code)
{
    size_t lo, hi;

    if (bank == NULL || code == NULL) return NULL;

    /* Entries are sorted by code (the generator sorts them), so this is a
     * binary search. This one IS on the speech path -- once per phoneme token
     * -- and the English bank has 40 entries, the Japanese ones 46. */
    lo = 0;
    hi = bank->phoneme_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int cmp = strcmp(code, bank->phonemes[mid].code);
        if (cmp == 0) return &bank->phonemes[mid];
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}
