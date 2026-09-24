/* kl_compile.h -- the schedule compiler, translated from `compileSection()`
 * and `compile()` in src/engine/sequencer.js.
 *
 * klattsch is Tony Gies's work and is MIT licensed; see LICENSE. This file is
 * a translation of part of it and carries the same notice.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 *
 * This is the stage where a difference is a *logic* difference rather than a
 * rounding one, so the translation is deliberately literal: the order of the
 * branches in renderPhoneme, the order of the three spreads in emit, and the
 * order the time accumulates in are all observable, and all reproduced as
 * written rather than tidied.
 *
 * Storage is the caller's, as everywhere else in this port: no malloc, so the
 * NVDA driver can compile an utterance on its synth thread without an
 * allocator in the path. kl_compile_need() sizes every buffer from the token
 * list, exactly rather than by a guessed constant.
 *
 * Strings are borrowed from the token list's arena and referred to by offset
 * and length. The token list must outlive the compiled result.
 */
#ifndef KL_COMPILE_H
#define KL_COMPILE_H

#include <stddef.h>
#include <stdint.h>

#include "kl_banks.h"
#include "kl_synth.h"
#include "kl_token.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- options ------------------------------------------------------------- */

/* The initial-state options compileSection reads with `opts.x ?? default`.
 * `present` says which were actually supplied, because "absent" and "supplied
 * as the default value" are the same thing to the JS and must stay the same
 * thing here. */
typedef enum {
    KL_OPT_BASE_F0 = 0,
    KL_OPT_RATE,
    KL_OPT_SCALE,
    KL_OPT_VIBRATO_DEPTH,
    KL_OPT_VIBRATO_RATE,
    KL_OPT_TREMOLO_DEPTH,
    KL_OPT_TREMOLO_RATE,
    KL_OPT_ASPIRATION,
    KL_OPT_TILT,
    KL_OPT_EFFORT,
    KL_OPT_COUNT
} kl_opt;

/* One entry of `opts.extras`: engine-specific state the caller seeds the
 * compile with. Keys are NUL-terminated and must outlive the result. */
typedef struct {
    const char *key;
    double value;
} kl_opt_extra;

typedef struct {
    uint32_t present;                  /* bitmask over kl_opt */
    double   value[KL_OPT_COUNT];

    const char *bank;                  /* NULL for the default bank        */
    const char *engine;                /* NULL, like `opts.engine ?? null` */

    const kl_opt_extra *extras;
    size_t n_extras;
} kl_compile_opts;

/* --- output -------------------------------------------------------------- */

/* One engine-specific extra riding in a schedule target: the `[OQ=0.6]` forms
 * the compiler accumulates and does not interpret. Only those whose key is
 * *not* one of the 19 synthesis parameters land here -- an extra named `F1`
 * overwrites the phoneme's F1 in the event itself, because that is what the
 * JS object spread does. */
typedef struct {
    const char *key;                   /* borrowed: token arena, or opts */
    uint32_t    key_len;               /* not NUL-terminated             */
    double      value;
} kl_extra;

/* Which slice of the extras pool an event's target carries. Events that share
 * an extras state share a span; the pool only grows when the state changes. */
typedef struct {
    uint32_t first, count;
} kl_extra_span;

/* A highlightable span of source with the time it sounds for. */
typedef struct {
    int32_t src_start, src_end;
    int32_t token_src_start;           /* the audible token within the span */
    double  t_start_ms, t_end_ms;
    kl_tok_type kind;                  /* phoneme, pause, or directive      */
    const char *phoneme;               /* borrowed; NULL when kind != phoneme */
    uint32_t    phoneme_len;
} kl_phrase;

typedef struct {
    const char *text;                  /* into the warning text arena */
    uint32_t    len;
} kl_warning;

/* One voice section's schedule. `events` is directly what kl_synth_queue()
 * wants; the spans beside it are for a consuming engine and are ignored by
 * the sample loop, exactly as the JS's extras are. */
typedef struct {
    kl_event      *events;
    kl_extra_span *spans;              /* parallel to events */
    size_t         n_events;

    kl_phrase *phrases;
    size_t     n_phrases;

    kl_warning *warnings;
    size_t      n_warnings;

    double total_ms;

    /* The engine marker in force at the end of the section. Borrowed from the
     * token arena when set by `[engine=x]`, from opts otherwise, NULL when
     * neither. `engine_len` is 0 and `engine` NULL when there is none. */
    const char *engine;
    size_t      engine_len;
} kl_voice;

typedef struct {
    kl_voice *voices;
    size_t    n_voices;

    /* The backward-compatible top level: voice 0's schedule and phrases, the
     * maximum totalMs across sections, and the warnings of every section
     * concatenated in section order. */
    double   total_ms;
    size_t   n_warnings;               /* summed over voices */

    /* Shared pools the voices point into. */
    kl_extra *extra_pool;
    size_t    n_extras;
    char     *text;                    /* warning text, UTF-8, not NUL-terminated */
    size_t    text_len;
} kl_compiled;

/* --- caller-provided storage --------------------------------------------- */

typedef struct {
    kl_voice      *voices;    size_t voices_cap;
    kl_event      *events;    size_t events_cap;
    kl_extra_span *spans;     /* events_cap entries */
    kl_extra      *extras;    size_t extras_cap;
    kl_phrase     *phrases;   size_t phrases_cap;
    kl_warning    *warnings;  size_t warnings_cap;
    char          *text;      size_t text_cap;

    /* Scratch. Not part of the result, but the compiler has nowhere else to
     * put it: no malloc, and C17 without variable-length arrays means the
     * caller owns even the temporaries.
     *   live -- the extras set as it currently stands, before it is sorted
     *           into the pool
     *   syl  -- the phonemes buffered inside `( ... )` until the group closes */
    kl_extra         *live;   size_t live_cap;
    const kl_token  **syl;    size_t syl_cap;
} kl_compile_arena;

/* Exact capacities for this token list. Every bound here is derived from what
 * the tokens actually are rather than from a guessed constant, because the
 * extras pool is the one place where a lazy bound would be quadratic in the
 * input length. */
void kl_compile_need(const kl_token_list *toks, const kl_compile_opts *opts,
                     kl_compile_arena *need);

#define KL_COMPILE_OK        0
#define KL_COMPILE_OVERFLOW (-1)
/* `resolveBank` throws on a name that is not registered; opts.bank is the one
 * place a caller can ask for a bank that does not exist, and the JS does not
 * reach a warning for it. A return, not a warning, so the two agree. */
#define KL_COMPILE_BAD_BANK (-2)

/* Compile a token list. `arena` must point at storage with at least the
 * capacities kl_compile_need() asks for. The token list is borrowed and must
 * outlive `out`. */
int kl_compile(const kl_token_list *toks, const kl_compile_opts *opts,
               kl_compile_arena *arena, kl_compiled *out);

/* The defaults in sequencer.js's DEFAULTS, exposed because the generator and
 * the documentation both quote them and a second copy would drift. */
#define KL_DEFAULT_BASE_F0                120.0
#define KL_DEFAULT_RATE                   110.0
#define KL_DEFAULT_STRESS_DURATION_FACTOR   1.5
#define KL_DEFAULT_STRESS_F0_LIFT           8.0
#define KL_DEFAULT_STOP_BURST_MS           25.0
#define KL_DEFAULT_TRANSITION_MS           35.0
#define KL_DEFAULT_SENTENCE_FINAL_HOLD_MS   0.0
#define KL_DEFAULT_FADE_OUT_MS            100.0
#define KL_DEFAULT_TRAIL_OFF_MS           150.0

#ifdef __cplusplus
}
#endif

#endif /* KL_COMPILE_H */
