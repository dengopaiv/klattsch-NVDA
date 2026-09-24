/* JavaScript number formatting, for the two numbers bin/klattsch.mjs prints.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */

#include "kl_fmt.h"

#include <math.h>
#include <stdio.h>

/* An exact tie at `digits` decimals is exactly the set x = odd / 2^(digits+1).
 *
 * A tie means x = (2k + 1) / (2 * 10^d). A double is a dyadic rational, so
 * the factor 5^d in the denominator has to divide 2k + 1, and what is left is
 * odd / 2^(d+1). ldexp by d+1 is exact, so the test needs no tolerance and no
 * reparsing of a printed string. */
static int is_tie(double x, int digits)
{
    double s = ldexp(x, digits + 1);
    return s == floor(s) && fmod(fabs(s), 2.0) == 1.0;
}

void kl_to_fixed(char *out, size_t cap, double x, int digits)
{
    if (x >= 0.0 && x < 1e21 && (digits == 0 || digits == 2) && is_tie(x, digits)) {
        /* x * 10^d is exactly an integer plus a half, so rounding the
         * magnitude up is exact arithmetic rather than a reprint. */
        double p10 = (digits == 0) ? 1.0 : 100.0;
        double n = floor(x * p10) + 1.0;
        if (digits == 0) snprintf(out, cap, "%.0f", n);
        else             snprintf(out, cap, "%.0f.%02.0f", floor(n / 100.0), fmod(n, 100.0));
        return;
    }
    snprintf(out, cap, "%.*f", digits, x);
}
