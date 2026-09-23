/* Phoneme banks: the parameter tables the compiler reads.
 *
 * Translated from src/engine/banks/index.js, with the data generated from
 * src/engine/banks/*.json by tools/build-banks-c.mjs. klattsch is Tony Gies's
 * work; a translation of someone's algorithm is still their algorithm, and the
 * phoneme data is somebody's published measurement -- see the `source` fields,
 * which are part of the data and are carried rather than dropped.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */
#ifndef KL_BANKS_H
#define KL_BANKS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The endpoint a diphthong glides towards. Only F1..F3 move; bandwidths and
 * amplitudes are held from the phoneme itself, which is what the JS `glideTo`
 * spread does. */
typedef struct {
    double F1, F2, F3;
} kl_glide;

/* One phoneme's parameters.
 *
 * The ten numeric fields are in the order the generator, the dump tool and the
 * verifier all agree on; changing it means changing all four together. */
typedef struct {
    const char *code;       /* ARPABET or bank-specific, NUL-terminated  */

    double voicing;
    double F1, F2, F3;
    double BW1, BW2, BW3;
    double A1, A2, A3;

    int is_stop;            /* silence-then-burst shape                  */
    int has_glide;
    kl_glide glide_to;

    /* Documentation and provenance. Not read by the synthesizer; carried
     * because dropping somebody's attribution to save a pointer is not a
     * trade this project makes. Any may be NULL. */
    const char *ipa;
    const char *example;
    const char *source;
} kl_phoneme;

/* A resolved bank: `extends` inheritance and `null` deletion have already been
 * applied by the generator, so `phonemes` is flat and complete. Entries are
 * sorted by `code`, which kl_bank_find relies on. */
typedef struct {
    const char *name;
    const char *display_name;
    const char *language;   /* may be NULL */
    const char *license;    /* may be NULL */
    const char *source;     /* may be NULL; provenance, not decoration */

    int schema_version;
    const kl_phoneme *phonemes;
    size_t phoneme_count;
} kl_bank;

/* The compiled-in banks, sorted by name. */
extern const kl_bank kl_banks[];
extern const size_t kl_bank_count;
extern const char *const kl_default_bank;

/* Look a bank up by name. NULL if there is no such bank. */
const kl_bank *kl_bank_get(const char *name);

/* The default bank, which is never NULL in a correctly generated build. */
const kl_bank *kl_bank_default(void);

/* Look a phoneme up within a bank. NULL if the bank does not define it --
 * which the compiler reports as `unknown phoneme: X` rather than treating as
 * silence. */
const kl_phoneme *kl_bank_find(const kl_bank *bank, const char *code);

#ifdef __cplusplus
}
#endif

#endif /* KL_BANKS_H */
