/* Stage 3 verification: render every schedule in goldens/schedules.bin and
 * emit the samples for tools/verify-stage3.mjs to compare.
 *
 *   kl_synth_dump <schedules.bin> <sample-rate> [--chunked N]
 *
 * Output, little-endian:
 *
 *   u32  case count
 *   per case:
 *     u32  id length, id bytes
 *     u32  sample count
 *     f32  samples
 *
 * `--chunked N` renders each case in N-sample pieces instead of one call.
 * The samples must come out identical either way -- the synth keeps its state
 * between calls -- and that is what the chunked rendering of phase 3 relies
 * on, so it is proved here rather than assumed later.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 */

#include "../kl_synth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

/* ---- reading the schedule file ---- */

typedef struct {
    const unsigned char *p;
    const unsigned char *end;
    int bad;
} reader;

static uint32_t rd_u32(reader *r)
{
    uint32_t v;
    if (r->end - r->p < 4) { r->bad = 1; return 0; }
    v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8)
      | ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    return v;
}

static double rd_f64(reader *r)
{
    uint64_t bits = 0;
    double v;
    int i;
    if (r->end - r->p < 8) { r->bad = 1; return 0.0; }
    for (i = 0; i < 8; i++) bits |= (uint64_t)r->p[i] << (8 * i);
    r->p += 8;
    memcpy(&v, &bits, sizeof v);
    return v;
}

/* ---- writing ---- */

static void put_u32(uint32_t v)
{
    unsigned char b[4];
    int i;
    for (i = 0; i < 4; i++) b[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
    fwrite(b, 1, 4, stdout);
}

static void put_f32(float v)
{
    uint32_t bits;
    unsigned char b[4];
    int i;
    memcpy(&bits, &v, sizeof bits);
    for (i = 0; i < 4; i++) b[i] = (unsigned char)((bits >> (8 * i)) & 0xFF);
    fwrite(b, 1, 4, stdout);
}

int main(int argc, char **argv)
{
    const char *path;
    double want_rate;
    long chunk = 0;
    FILE *f;
    long file_len;
    unsigned char *data;
    reader r;
    uint32_t version, rate_count, case_count, i;

    if (argc < 3) {
        fprintf(stderr, "usage: kl_synth_dump <schedules.bin> <sample-rate> [--chunked N]\n");
        return 2;
    }
    path = argv[1];
    want_rate = atof(argv[2]);
    if (argc >= 5 && strcmp(argv[3], "--chunked") == 0) chunk = atol(argv[4]);

#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    file_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (unsigned char *)malloc((size_t)file_len);
    if (!data) { fclose(f); fprintf(stderr, "out of memory\n"); return 1; }
    if (fread(data, 1, (size_t)file_len, f) != (size_t)file_len) {
        fclose(f); free(data); fprintf(stderr, "short read\n"); return 1;
    }
    fclose(f);

    r.p = data;
    r.end = data + file_len;
    r.bad = 0;

    if (r.end - r.p < 8 || memcmp(r.p, "KLSCHED\0", 8) != 0) {
        fprintf(stderr, "not a schedule file\n"); free(data); return 1;
    }
    r.p += 8;
    version = rd_u32(&r);
    if (version != 1) { fprintf(stderr, "unsupported version %u\n", version); free(data); return 1; }

    rate_count = rd_u32(&r);
    for (i = 0; i < rate_count; i++) (void)rd_f64(&r);

    case_count = rd_u32(&r);
    put_u32(case_count);

    for (i = 0; i < case_count; i++) {
        uint32_t id_len, voice_count, v, n_events, e;
        const unsigned char *id;
        double total_ms;
        kl_event *events = NULL;
        long *at = NULL, *tl = NULL;
        uint32_t n_events_v0 = 0;
        size_t sample_count;
        float *buf;
        kl_synth s;

        id_len = rd_u32(&r);
        id = r.p;
        r.p += id_len;
        total_ms = rd_f64(&r);
        voice_count = rd_u32(&r);

        put_u32(id_len);
        fwrite(id, 1, id_len, stdout);

        /* Voice 0 only: that is what the audio goldens were rendered from
         * (renderToBuffer takes compiled.schedule, which mirrors voice 0),
         * and mixing voices is the CLI's job, not the synth's. Later voices
         * are read past so the stream stays aligned. */
        for (v = 0; v < voice_count; v++) {
            n_events = rd_u32(&r);
            if (v == 0) {
                events = (kl_event *)calloc(n_events ? n_events : 1, sizeof *events);
                at = (long *)calloc(n_events ? n_events : 1, sizeof *at);
                tl = (long *)calloc(n_events ? n_events : 1, sizeof *tl);
                if (!events || !at || !tl) { fprintf(stderr, "out of memory\n"); free(data); return 1; }
            }
            for (e = 0; e < n_events; e++) {
                double at_ms = rd_f64(&r);
                double tr_ms = rd_f64(&r);
                uint32_t mask = rd_u32(&r);
                int k;
                for (k = 0; k < KL_PARAM_COUNT; k++) {
                    if (mask & (1u << k)) {
                        const double val = rd_f64(&r);
                        if (v == 0) events[e].value[k] = val;
                    }
                }
                if (v == 0) {
                    events[e].at_ms = at_ms;
                    events[e].transition_ms = tr_ms;
                    events[e].present = mask;
                }
            }
            if (v == 0) {
                kl_schedule_prepare(events, n_events, want_rate, at, tl);
                n_events_v0 = n_events;
            }
        }

        /* Math.ceil(totalMs * sampleRate / 1000), as renderToBuffer does. */
        sample_count = (size_t)ceil(total_ms * want_rate / 1000.0);
        buf = (float *)calloc(sample_count ? sample_count : 1, sizeof *buf);
        if (!buf) { fprintf(stderr, "out of memory\n"); free(data); return 1; }

        {
            kl_synth_init(&s, want_rate, NULL, 0);
            kl_synth_queue(&s, events, n_events_v0, at, tl, 0);
            if (chunk > 0) {
                size_t done = 0;
                while (done < sample_count) {
                    size_t take = sample_count - done;
                    if (take > (size_t)chunk) take = (size_t)chunk;
                    kl_synth_process(&s, buf + done, take);
                    done += take;
                }
            } else {
                kl_synth_process(&s, buf, sample_count);
            }
        }

        put_u32((uint32_t)sample_count);
        {
            size_t j;
            for (j = 0; j < sample_count; j++) put_f32(buf[j]);
        }

        free(buf);
        free(events);
        free(at);
        free(tl);
    }

    free(data);
    if (r.bad) { fprintf(stderr, "truncated schedule file\n"); return 1; }
    return ferror(stdout) ? 1 : 0;
}
