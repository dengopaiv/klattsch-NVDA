/* Stage 1 verification: emit the DSP primitives as raw little-endian bytes,
 * in exactly the order tools/goldens.mjs digests them.
 *
 * This writes bytes rather than computing a hash, so there is no SHA-256 in C
 * to get wrong, and so the tolerance comparisons (which a hash cannot express)
 * can work on the values themselves. tools/verify-stage1.mjs hashes the exact
 * streams and value-compares the transcendental ones.
 *
 *   kl_dsp_dump lfsr      1,000,000 int32 LE, from seed 0xACE1ACE1
 *   kl_dsp_dump softclip  6001 f64 LE, x = -3.000 .. 3.000 step 0.001
 *   kl_dsp_dump pulse     101 x 1000 f64 LE, effort outer, phase inner
 *   kl_dsp_dump biquad    (sr, f, bw) grid, 5 coefficients each
 *   kl_dsp_dump cache     the coefficient-cache probe, 9 f64 LE
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */

#include "../kl_dsp.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

/* Write a double as 8 little-endian IEEE-754 bytes, byte by byte, so the
 * output does not depend on the host's endianness or struct padding. */
static void put_f64(double v)
{
    uint64_t bits;
    unsigned char b[8];
    int i;
    memcpy(&bits, &v, sizeof bits);
    for (i = 0; i < 8; i++) b[i] = (unsigned char)((bits >> (8 * i)) & 0xFF);
    fwrite(b, 1, 8, stdout);
}

static void put_i32(int32_t v)
{
    uint32_t u = (uint32_t)v;
    unsigned char b[4];
    int i;
    for (i = 0; i < 4; i++) b[i] = (unsigned char)((u >> (8 * i)) & 0xFF);
    fwrite(b, 1, 4, stdout);
}

static void dump_lfsr(void)
{
    int32_t state = (int32_t)0xACE1ACE1u;
    long i;
    for (i = 0; i < 1000000L; i++) {
        state = kl_xorshift(state);
        put_i32(state);
    }
}

static void dump_softclip(void)
{
    int i;
    for (i = -3000; i <= 3000; i++) put_f64(kl_soft_clip((double)i / 1000.0));
}

static void dump_pulse(void)
{
    int e, p;
    for (e = 0; e <= 100; e++) {
        for (p = 0; p < 1000; p++) {
            put_f64(kl_glottal_pulse((double)p / 1000.0, (double)e / 100.0));
        }
    }
}

/* The same grid tools/goldens.mjs walks, including both clamp regions. */
static void dump_biquad(void)
{
    static const double rates[] = { 8000.0, 22050.0, 44100.0, 48000.0 };
    static const double bws[] = { 0.0, 10.0, 19.0, 20.0, 21.0, 50.0, 100.0, 200.0, 400.0, 1000.0 };
    size_t ri, fi, bi;
    for (ri = 0; ri < sizeof rates / sizeof rates[0]; ri++) {
        const double sr = rates[ri];
        const double fs[] = {
            0.0, 10.0, 39.0, 40.0, 41.0, 100.0, 500.0, 1500.0, 2500.0, 3500.0,
            sr * 0.44, sr * 0.45, sr * 0.46, sr
        };
        for (fi = 0; fi < sizeof fs / sizeof fs[0]; fi++) {
            for (bi = 0; bi < sizeof bws / sizeof bws[0]; bi++) {
                kl_biquad b;
                kl_biquad_init(&b);
                kl_biquad_set(&b, fs[fi], bws[bi], sr);
                put_f64(b.b0); put_f64(b.b1); put_f64(b.b2);
                put_f64(b.a1); put_f64(b.a2);
            }
        }
    }
}

/* The cache keys on raw (f, bw) before clamping. Two different raw pairs that
 * clamp to the same thing must each recompute; a repeat of the same raw pair
 * must not. Three coefficient triples, same probe as the JS. */
static void dump_cache(void)
{
    kl_biquad b;
    kl_biquad_init(&b);
    kl_biquad_set(&b, 10.0, 5.0, 48000.0);   /* clamps to (40, 20) */
    put_f64(b.b0); put_f64(b.a1); put_f64(b.a2);
    kl_biquad_set(&b, 20.0, 8.0, 48000.0);   /* also clamps to (40, 20) */
    put_f64(b.b0); put_f64(b.a1); put_f64(b.a2);
    kl_biquad_set(&b, 20.0, 8.0, 48000.0);   /* cache hit: unchanged */
    put_f64(b.b0); put_f64(b.a1); put_f64(b.a2);
}

int main(int argc, char **argv)
{
#if defined(_WIN32)
    /* Without this the CRT translates 0x0A to 0x0D 0x0A on the way out and
     * every digest is wrong for a reason that looks like a DSP bug. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc != 2) {
        fprintf(stderr, "usage: kl_dsp_dump <lfsr|softclip|pulse|biquad|cache>\n");
        return 2;
    }
    if      (strcmp(argv[1], "lfsr")     == 0) dump_lfsr();
    else if (strcmp(argv[1], "softclip") == 0) dump_softclip();
    else if (strcmp(argv[1], "pulse")    == 0) dump_pulse();
    else if (strcmp(argv[1], "biquad")   == 0) dump_biquad();
    else if (strcmp(argv[1], "cache")    == 0) dump_cache();
    else {
        fprintf(stderr, "unknown section: %s\n", argv[1]);
        return 2;
    }
    return ferror(stdout) ? 1 : 0;
}
