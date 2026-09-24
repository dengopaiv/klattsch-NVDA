/* Minimal RIFF/WAVE encoder with peak normalization.
 *
 * Translated from src/engine/wav.js. klattsch is Tony Gies's work; a
 * translation of someone's algorithm is still their algorithm.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */

#include "kl_wav.h"

#include <math.h>
#include <string.h>

/* JavaScript truthiness for a number: 0, -0 and NaN are false, everything
 * else is true. `if (peakNormalize)` is the only place it matters, but the
 * C spelling of it is not obvious -- `if (x)` in C is true for NaN. */
static int js_truthy(double x)
{
    return !(x == 0.0 || isnan(x));
}

double kl_wav_round(double x)
{
    double r, d, y;

    if (isnan(x) || isinf(x)) return x;

    r = floor(x);
    /* x - floor(x) is exact for every finite x: for |x| >= 1 the two are
     * within a factor of two of each other (Sterbenz), and for |x| < 1 the
     * floor is 0 or -1 and the subtraction is trivially exact. That is what
     * lets the tie be tested with == rather than with a tolerance. */
    d = x - r;
    y = (d >= 0.5) ? r + 1.0 : r;

    /* Math.round returns -0 for every x in [-0.5, -0]. The value is the same
     * number, but Object.is distinguishes them and the sweep in
     * tools/verify-stage6.mjs compares with Object.is. */
    if (y == 0.0) y = signbit(x) ? -0.0 : 0.0;
    return y;
}

double kl_wav_gain(const float *buf, size_t n, double peak_normalize)
{
    double gain = 1.0;

    if (js_truthy(peak_normalize)) {
        double peak = 0.0;
        size_t i;
        for (i = 0; i < n; i++) {
            double v = (double)buf[i];
            double a = v < 0.0 ? -v : v;
            if (a > peak) peak = a;
        }
        if (peak > 0.0) gain = peak_normalize / peak;
    }
    return gain;
}

/* Sizes of the two INFO sub-chunk payloads, after the JavaScript's falsy
 * test: a NULL or empty string is absent, not present-and-empty. Returns the
 * total size of the LIST chunk including its own 8-byte header, or 0 when
 * there is nothing to write -- `if (!subs.length) return null`. */
static size_t info_size(const kl_wav_meta *meta, size_t *sw_len, size_t *cm_len)
{
    size_t s = 0, c = 0, payload = 4, subs = 0;

    if (meta) {
        if (meta->software && meta->software[0]) s = strlen(meta->software);
        if (meta->comment  && meta->comment[0])  c = strlen(meta->comment);
    }
    if (s) { payload += 8 + s + (s % 2); subs++; }
    if (c) { payload += 8 + c + (c % 2); subs++; }

    if (sw_len) *sw_len = s;
    if (cm_len) *cm_len = c;
    return subs ? 8 + payload : 0;
}

size_t kl_wav_size(size_t n_samples, const kl_wav_meta *meta)
{
    return 44 + n_samples * 2 + info_size(meta, NULL, NULL);
}

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

/* One INFO sub-chunk: four-character id, unpadded length, data, then the pad
 * byte when the length is odd. The pad is *not* counted in the sub-chunk's
 * own size field but *is* counted in the LIST payload size -- which is what
 * the `oddComment` golden exists to pin down. */
static size_t put_sub(unsigned char *p, const char *id, const char *data, size_t len)
{
    memcpy(p, id, 4);
    put_u32(p + 4, (uint32_t)len);
    memcpy(p + 8, data, len);
    if (len % 2) {
        p[8 + len] = 0;
        return 8 + len + 1;
    }
    return 8 + len;
}

size_t kl_wav_encode(const float *buf, size_t n, uint32_t sample_rate,
                     double peak_normalize, const kl_wav_meta *meta,
                     unsigned char *out, size_t out_cap, double *gain_out)
{
    size_t sw_len = 0, cm_len = 0;
    size_t list_bytes = info_size(meta, &sw_len, &cm_len);
    size_t data_bytes = n * 2;
    size_t total = 44 + data_bytes + list_bytes;
    double gain;
    size_t i, o;

    if (out_cap < total) return 0;

    gain = kl_wav_gain(buf, n, peak_normalize);
    if (gain_out) *gain_out = gain;

    memcpy(out + 0, "RIFF", 4);
    put_u32(out + 4, (uint32_t)(total - 8));
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    put_u32(out + 16, 16);                          /* PCM fmt chunk size */
    put_u16(out + 20, 1);                           /* format = PCM */
    put_u16(out + 22, 1);                           /* mono */
    put_u32(out + 24, sample_rate);
    put_u32(out + 28, sample_rate * 2);              /* byte rate */
    put_u16(out + 32, 2);                           /* block align */
    put_u16(out + 34, 16);                          /* bits per sample */
    memcpy(out + 36, "data", 4);
    put_u32(out + 40, (uint32_t)data_bytes);

    o = 44;
    for (i = 0; i < n; i++) {
        double s = (double)buf[i] * gain;
        double q;
        int32_t v;

        if (s > 1.0) s = 1.0;
        else if (s < -1.0) s = -1.0;

        q = kl_wav_round(s * 32767.0);
        /* DataView.setInt16 applies ToInt16, which maps NaN to +0. The clamp
         * above has already turned both infinities into +/-1, so NaN is the
         * only non-finite value that can reach here -- but casting it to
         * int32_t would be undefined behaviour, so it is handled rather than
         * assumed away. */
        v = isfinite(q) ? (int32_t)q : 0;

        out[o]     = (unsigned char)((uint32_t)v & 0xFFu);
        out[o + 1] = (unsigned char)(((uint32_t)v >> 8) & 0xFFu);
        o += 2;
    }

    if (list_bytes) {
        unsigned char *p = out + o;
        memcpy(p, "LIST", 4);
        put_u32(p + 4, (uint32_t)(list_bytes - 8));
        memcpy(p + 8, "INFO", 4);
        p += 12;
        /* ISFT before ICMT: the order the JavaScript pushes them, and the
         * order the bytes are compared in. */
        if (sw_len) p += put_sub(p, "ISFT", meta->software, sw_len);
        if (cm_len) p += put_sub(p, "ICMT", meta->comment,  cm_len);
        o += list_bytes;
    }

    return total;
}
