/* kl_text_rules.h -- declarations for the letter-to-sound tables.  The data,
 * and where it came from, are in kl_text_rules.c.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * See NOTICE.md.
 */
#ifndef KL_TEXT_RULES_H
#define KL_TEXT_RULES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One NRL letter-to-sound rule: at the cursor, if `match` is present and both
 * contexts hold, emit `out` and advance by strlen(match).  `left` is written
 * in reverse reading order, so the character nearest the cursor comes last. */
typedef struct {
    const char *left;
    const char *match;
    const char *right;
    const char *out;
} kl_lts_rule;

/* The rules for one leading character, in priority order. */
typedef struct {
    const kl_lts_rule *rules;
    size_t count;
} kl_lts_rule_group;

/* Indexed 0 for punctuation, then 1 + (letter - 'A'). */
extern const kl_lts_rule_group KL_LTS_NRL_RULES[27];

/* Whole-word rewrites applied before the rules run, as {as written, as
 * respelled}.  Both are space-padded so the blanks act as word boundaries.
 * Abbreviations are expanded first, so the expansion is then subject to the
 * ordinary rules. */
extern const char *const KL_LTS_ABBREVIATIONS[][2];
extern const size_t KL_LTS_ABBREVIATION_COUNT;
extern const char *const KL_LTS_EXCEPTIONS[][2];
extern const size_t KL_LTS_EXCEPTION_COUNT;

/* Number names as ARPABET.  [0..19] are zero..nineteen, [20..27] twenty,
 * thirty..ninety. */
extern const char *const KL_LTS_CARDINALS[28];
extern const char *const KL_LTS_ORDINALS[28];

/* Spoken names for the 128 ASCII codes, indexed by the character's own code. */
extern const char *const KL_LTS_ASCII_NAMES[128];

/* The words the number reader adds around the cardinals: the scale names,
 * the decimal point, and the currency words. */
extern const char *const KL_LTS_HUNDRED;
extern const char *const KL_LTS_THOUSAND;
extern const char *const KL_LTS_MILLION;
extern const char *const KL_LTS_BILLION;
extern const char *const KL_LTS_POINT;
extern const char *const KL_LTS_DOLLAR;
extern const char *const KL_LTS_DOLLARS;
extern const char *const KL_LTS_AND;
extern const char *const KL_LTS_CENT;
extern const char *const KL_LTS_CENTS;

#ifdef __cplusplus
}
#endif

#endif /* KL_TEXT_RULES_H */
