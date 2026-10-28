/* kl_dict.h -- the CMU Pronouncing Dictionary, compiled in.
 *
 * The whole of data/cmudict/cmudict.dict (cmusphinx/cmudict at 7479086),
 * every entry including the alternative pronunciations ("READ(2)") and the
 * keys that are not plain words, turned into a C table at build time by
 * csrc/tools/kl_dict_gen.c. The front end looks words up here before the
 * letter-to-sound rules (kl_text.c, pass 1); see docs/23-dictionary.md.
 *
 * Each phone is stored as one character: an index into kl_dict_symbols, the
 * 24 consonants and the 15 vowels with each of CMU's three stress digits --
 * 69 symbols. They are CMU's own names, and CMU's 39 phones are exactly the
 * klatt1980-en bank's 39, so nothing is mapped.
 *
 * The CMU Pronouncing Dictionary is copyright (C) 1993-2015 Carnegie Mellon
 * University, under the BSD-style terms in data/cmudict/LICENSE, which every
 * binary built from it carries (NOTICE.md).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Päiv Dengo (this code; the data is CMU's)
 */
#ifndef KL_DICT_H
#define KL_DICT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KL_DICT_SYMBOL_COUNT 69

/* "B", "CH", ... "ZH", then "AA0", "AA1", "AA2", ... "UW2". */
extern const char *const kl_dict_symbols[KL_DICT_SYMBOL_COUNT];

/* The character that stands for symbol `i` in the table, and back: printable
 * ASCII from '#', skipping '?' (a trigraph's start in C17) and '\\', so the
 * generated string literals need no escapes. -1 for a character that is not
 * a code. */
char kl_dict_code_char(int i);
int  kl_dict_code_index(char c);

/* The symbol named `name` ("AH0", "K"), or -1. */
int kl_dict_symbol_index(const char *name, size_t len);

/* Look up `key`, `len` bytes, upper case, exactly as the table spells it:
 * "HELLO", "JOHN'S", "READ(2)". Returns the entry's codes -- one character
 * per phone, not NUL-terminated -- and their count in *n, or NULL. */
const char *kl_dict_find(const char *key, size_t len, size_t *n);

/* Every entry, in key order, for verification: the key and its length, and
 * the codes and their count. */
size_t kl_dict_count(void);
void   kl_dict_entry(size_t i, const char **key, size_t *key_len,
                     const char **codes, size_t *n);

#ifdef __cplusplus
}
#endif

#endif /* KL_DICT_H */
