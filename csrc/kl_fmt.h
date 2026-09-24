/* JavaScript number formatting, for the two numbers bin/klattsch.mjs prints.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 *
 * This exists because printf and Number.prototype.toFixed round differently,
 * and both disagreements are reachable in the CLI's one line of output:
 *
 *   printf("%.*f")  rounds the exact binary value to nearest, ties to even.
 *   toFixed         strips the sign and rounds the magnitude, ties away.
 *
 * `(2.5).toFixed(0)` is "3" where printf gives "2", and `(0.375).toFixed(2)`
 * is "0.38" where printf gives "0.38" but `(0.125).toFixed(2)` is "0.13"
 * where printf gives "0.12". A file 512 bytes mod 1024 makes `bytes / 1024`
 * an exact half; a 375 ms utterance makes `totalMs / 1000` exactly 0.375.
 *
 * It lives in the library rather than inside the CLI so that it can be swept
 * against the reference by csrc/tools/kl_wav_dump.c --tofixed-sweep. A helper
 * that is only right for the arguments it happens to get is what a later
 * change breaks silently.
 */
#ifndef KL_FMT_H
#define KL_FMT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number.prototype.toFixed(digits), for finite x in [0, 1e21) and digits of
 * 0 or 2 -- the only values bin/klattsch.mjs uses.
 *
 * Outside that domain it falls back to printf, which is right everywhere
 * except on an exact tie; the restriction is stated rather than silently
 * assumed because the CLI's three arguments (a byte count, a duration and a
 * normalization gain) are all non-negative and none can approach 1e21. */
void kl_to_fixed(char *out, size_t cap, double x, int digits);

#ifdef __cplusplus
}
#endif

#endif /* KL_FMT_H */
