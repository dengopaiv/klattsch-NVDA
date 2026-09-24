/* Minimal RIFF/WAVE encoder with peak normalization.
 *
 * Translated from src/engine/wav.js. klattsch is Tony Gies's work; a
 * translation of someone's algorithm is still their algorithm.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 *
 * Three things about this file are contracts rather than implementation
 * details, because the bytes they produce are compared against the
 * JavaScript's byte for byte:
 *
 *  1. The LIST INFO chunk comes *after* the data chunk, holds ISFT before
 *     ICMT, and pads each sub-chunk to an even length with a zero byte that
 *     is not counted in that sub-chunk's own size field.
 *  2. Normalization reads the peak of the whole buffer before any sample is
 *     written, so every sample in the file depends on one sample of the
 *     input. That is upstream's design; it is also why stage 6 had to measure
 *     whether a sub-ULP disagreement can move the peak. See
 *     docs/18-stage6-wav.md.
 *  3. Rounding is JavaScript's Math.round, which is neither C's round() nor
 *     floor(x + 0.5). kl_wav_round() is exported so the difference can be
 *     swept against the reference rather than asserted.
 */
#ifndef KL_WAV_H
#define KL_WAV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ISFT field bin/klattsch.mjs:37 writes into every file it renders.
 *
 * Spelled with a hex escape rather than a literal U+00B7 so that no
 * compiler's source-charset default can alter it: MSVC without /utf-8 reads
 * a .c file in the system ANSI codepage, and this string is attribution --
 * the one thing in the output that must survive a toolchain change intact.
 * "\xC2\xB7" is the UTF-8 encoding of U+00B7 MIDDLE DOT; the character after
 * it is a space, so the hex escape cannot swallow another digit.
 *
 * tools/verify-stage6.mjs checks this against bin/klattsch.mjs, so the two
 * cannot drift apart silently. */
#define KL_WAV_SOFTWARE "klattsch \xC2\xB7 https://tgies.github.io/klattsch"

/* encodeWav's default peakNormalize. */
#define KL_WAV_PEAK_NORMALIZE 0.95

/* The LIST INFO fields. A NULL or empty string is omitted, matching the
 * JavaScript's `if (metadata.software)` -- in JS an empty string is falsy, so
 * "present but empty" and "absent" are the same thing and must stay so. A
 * NULL `kl_wav_meta *` omits the chunk entirely, as `metadata: null` does. */
typedef struct {
    const char *software;   /* ISFT */
    const char *comment;    /* ICMT */
} kl_wav_meta;

/* JavaScript's Math.round: round to nearest, ties toward +Infinity.
 *
 * Not C's round(), which breaks ties away from zero -- Math.round(-1.5) is
 * -1 where round(-1.5) is -2. Not floor(x + 0.5) either: that returns 1 for
 * 0.49999999999999994, because the addition rounds up, while Math.round
 * returns 0. Exported so tools/verify-stage6.mjs can sweep it. */
double kl_wav_round(double x);

/* The normalization gain encodeWav would apply. Separate from the encoder
 * because the CLI prints it and the verifier compares it as an exact double;
 * a gain that differs is a different file in every sample, so it is worth
 * seeing on its own. */
double kl_wav_gain(const float *buf, size_t n, double peak_normalize);

/* Bytes kl_wav_encode will write for this many samples and this metadata.
 * Exact, not an upper bound. */
size_t kl_wav_size(size_t n_samples, const kl_wav_meta *meta);

/* Encode. Returns the number of bytes written, or 0 if `out_cap` is smaller
 * than kl_wav_size() -- nothing is written in that case. `gain_out` may be
 * NULL. `peak_normalize` of 0 (or NaN, which is falsy in JavaScript) disables
 * normalization, exactly as `{ peakNormalize: 0 }` does. */
size_t kl_wav_encode(const float *buf, size_t n, uint32_t sample_rate,
                     double peak_normalize, const kl_wav_meta *meta,
                     unsigned char *out, size_t out_cap, double *gain_out);

#ifdef __cplusplus
}
#endif

#endif /* KL_WAV_H */
