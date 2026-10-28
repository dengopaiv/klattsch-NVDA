/* klattsch_api.h -- the streaming API: what the NVDA add-on calls.
 *
 * The engine, the compiler and the text front end are verified pieces, and
 * the generator strings them together for a whole utterance at a time. A
 * screen reader needs two things that path does not have: the first sample
 * before the last one is rendered, and NVDA's index markers placed on the
 * audio. This adds those two things and nothing else. No sample the goldens
 * pin is changed: the float stream is kl_render_mix()'s output, chunk by
 * chunk, and docs/21-stage-api.md says how that was proved.
 *
 * Shape of a call sequence, on one thread:
 *
 *   h = kl_api_create(22050);
 *   kl_api_set(h, KL_SET_RATE, 90);
 *   kl_api_begin(h);
 *   kl_api_text(h, "Hello there.");   kl_api_index(h, 1);
 *   kl_api_text(h, "Second line.");   kl_api_index(h, 2);
 *   kl_api_end(h);
 *   while ((n = kl_api_render(h, buf, 1024, &index)) > 0 || index >= 0) {
 *       ... play n samples; if index >= 0, it is reached after them ...
 *   }
 *
 * Only pointers, ints and doubles cross this boundary, so a ctypes caller
 * needs no structures, and nothing here calls back into the caller.
 *
 * One handle belongs to one thread. Two handles share nothing and may run on
 * two threads.
 *
 * Not a translation of klattsch -- upstream has no such API -- but it drives
 * the translated engine, so it carries the engine's notice as well as the
 * front end's: see NOTICE.md.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies (the engine this drives)
 * Copyright (c) 2026 Päiv Dengo (this API)
 */
#ifndef KLATTSCH_API_H
#define KLATTSCH_API_H

#include <stdint.h>

#if defined(_WIN32) && defined(KL_API_SHARED_BUILD)
#  define KL_API __declspec(dllexport)
#elif defined(_WIN32) && defined(KL_API_SHARED)
#  define KL_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(KL_API_SHARED_BUILD)
#  define KL_API __attribute__((visibility("default")))
#else
#  define KL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kl_api kl_api;

/* KL_VERSION_STRING of csrc/kl_version.h, so a caller can refuse a library
 * that is not the one its manifest names. */
KL_API const char *kl_api_version(void);

/* A handle rendering at `sample_rate` Hz, with every setting at its default.
 * NULL when the rate is not positive or memory runs out. The rate is fixed
 * for the handle's life; a caller changing it makes a new handle. */
KL_API kl_api *kl_api_create(int sample_rate);
KL_API void    kl_api_destroy(kl_api *h);
KL_API int     kl_api_sample_rate(const kl_api *h);

/* --- settings ------------------------------------------------------------ */

/* The first ten are the compiler's options, in kl_opt order, and go to it
 * unchanged. The rest belong to the front end and to this API. A setting
 * takes effect at the next kl_api_begin(): an utterance already built keeps
 * the settings it was built with. */
enum {
    KL_SET_BASE_F0 = 0,      /* Hz; default 120                               */
    KL_SET_RATE,             /* ms per phoneme; default 110; smaller is faster */
    KL_SET_SCALE,            /* formant and bandwidth scale; default 1        */
    KL_SET_VIBRATO_DEPTH,    /* Hz; default 0                                 */
    KL_SET_VIBRATO_RATE,     /* Hz; default 5                                 */
    KL_SET_TREMOLO_DEPTH,    /* 0..1; default 0                               */
    KL_SET_TREMOLO_RATE,     /* Hz; default 5                                 */
    KL_SET_ASPIRATION,       /* 0..1; default 0                               */
    KL_SET_TILT,             /* -0.95..0.95; default 0                        */
    KL_SET_EFFORT,           /* 0..1; default 0.5                             */
    KL_SET_INFLECTION,       /* contour scale, >= 0; default 1                */
    KL_SET_VOLUME,           /* output scale, >= 0; default 1                 */
    KL_SET_COMMA_MS,         /* comma pause, ms; 0 means the front end's 200  */
    KL_SET_COUNT
};

#define KL_API_OK          0
#define KL_API_BAD_ARG    (-1)   /* unknown setting, or a value out of range  */
#define KL_API_NO_MEMORY  (-2)
#define KL_API_BAD_STATE  (-3)   /* called between the wrong pair of calls     */
#define KL_API_BAD_BANK   (-4)
#define KL_API_FAILED     (-5)   /* the tokenizer or compiler refused          */

/* Non-finite values are refused everywhere. Rate, base F0 and scale must be
 * positive; inflection, volume and the comma pause must not be negative. The
 * others are passed to the compiler as they are, as the generator does. */
KL_API int    kl_api_set(kl_api *h, int setting, double value);
KL_API double kl_api_get(const kl_api *h, int setting);

/* The phoneme bank the compiler starts from; NULL or "" for its default,
 * klatt1980-en. Refused when the bank is not compiled in. The front end
 * writes klatt1980-en's ARPABET only, so another bank makes text come out
 * silent: this exists for presets and tests, and the NVDA driver offers no
 * bank setting (docs/NVDA-ADDON.md). */
KL_API int kl_api_set_bank(kl_api *h, const char *name);

/* --- building an utterance ---------------------------------------------- */

/* Start an utterance, dropping any that was being built or rendered. */
KL_API int kl_api_begin(kl_api *h);

/* Pieces, in order. Text goes through the front end, with its contour;
 * spelling reads each character by name; source is klattsch source exactly
 * as typed into the generator; a break is a pause of `ms` milliseconds. */
KL_API int kl_api_text(kl_api *h, const char *utf8);
KL_API int kl_api_spell(kl_api *h, const char *utf8);
KL_API int kl_api_source(kl_api *h, const char *utf8);
KL_API int kl_api_break(kl_api *h, int ms);

/* An index after the pieces given so far. Ids are the caller's; any int but
 * -1, which kl_api_render uses for "none". */
KL_API int kl_api_index(kl_api *h, int index);

/* Compile what was built. Returns the number of samples the utterance will
 * render, or a negative KL_API_* code. Returns 0 when there is nothing to
 * hear and no pause to wait for -- no pieces, or only indexes -- in which
 * case rendering yields the indexes and no audio. */
KL_API long kl_api_end(kl_api *h);

/* How many phonemes of the compiled utterance make a sound: known to the
 * bank, given time, and not the silence `_`. Text with letters in it that
 * compiles to 0 here is the "silent success" of SCREEN-READER.md §4, and the
 * caller should treat it as a fault. */
KL_API long kl_api_sounding(const kl_api *h);

/* The compiler's warnings for the compiled utterance, joined with ", ", as
 * the generator shows them. Valid until the next kl_api_begin. */
KL_API const char *kl_api_warnings(const kl_api *h);

/* --- rendering ----------------------------------------------------------- */

/* Render the next chunk: at most `cap` samples, 16-bit, mono, at the
 * handle's rate. A chunk stops early at an index, so the caller can attach
 * it to exactly the audio before it: *index is then that index, else -1.
 * Returns the samples written. Indexes at the same position, and any left
 * at the end, come out one per call with 0 samples. 0 samples with *index
 * == -1 means the utterance is finished; every index has then come out
 * exactly once.
 *
 * Level: the engine's float output (soft-clipped, so below 1 in magnitude)
 * times KL_API_OUTPUT_GAIN times the volume setting, clamped to [-1, 1] and
 * rounded to 16 bits the way kl_wav does. There is no peak normalization: a
 * stream cannot know its peak before its end. */
KL_API int kl_api_render(kl_api *h, int16_t *out, int cap, int *index);

/* The same stream before the level: the float mix exactly as
 * kl_render_mix() would have produced it, chunk by chunk. For tests and for
 * callers that want to do their own level. Either call may be used for any
 * chunk; they share one position. */
KL_API int kl_api_render_float(kl_api *h, float *out, int cap, int *index);

/* Stop the utterance being rendered. The next kl_api_render calls yield no
 * more audio, only the indexes that had not yet come out, then the end. */
KL_API void kl_api_cancel(kl_api *h);

/* The fixed output gain of kl_api_render. Measured, not chosen: see
 * docs/21-stage-api.md. */
KL_API double kl_api_output_gain(void);

#ifdef __cplusplus
}
#endif

#endif /* KLATTSCH_API_H */
