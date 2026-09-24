/* kl_compile.c -- the schedule compiler.
 *
 * Translated from `compileSection()` and `compile()` in src/engine/sequencer.js.
 * klattsch is Tony Gies's work; a translation of someone's algorithm is still
 * their algorithm.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Tony Gies
 *
 * Read docs/17-stage5-compile.md before changing anything here. Three orders
 * in this file are observable in the output and none of them is arbitrary:
 *
 *   1. The branch order in render_phoneme(): stop, then glide, then pitch
 *      move, then steady. A phoneme that was both a stop and a glide would
 *      take the stop shape. No bank has one; the order is still the contract.
 *   2. The three layers in emit(): the phoneme's own parameters, then the
 *      accumulated extras, then the running voice state. Each overwrites the
 *      one before it. That is what the JS object spread does, and it is why
 *      `[F1=900]` beats the phoneme's F1 and why nothing can override the
 *      seven voice-state parameters.
 *   3. The order time accumulates in. `timeMs += slotMs` repeated in float64
 *      is not the same number as an index times a slot width, and the
 *      schedule is compared as exact IEEE-754 doubles.
 */
#include "kl_compile.h"

#include <math.h>
#include <string.h>

/* --- small helpers -------------------------------------------------------- */

/* Math.min, not fmin. They disagree on NaN (Math.min propagates it, fmin
 * returns the other operand) and on signed zero (Math.min(-0,+0) is -0).
 * Neither case is reachable here -- every call passes a positive literal as
 * the first argument -- but a helper that is only right for the arguments it
 * happens to get is what a later change breaks silently. */
static double js_min(double a, double b)
{
    if (a != a) return a;
    if (b != b) return b;
    if (a == 0.0 && b == 0.0) return signbit(a) ? a : b;
    return a < b ? a : b;
}

/* strcmp ordering between a NUL-terminated string and a counted one. The
 * generated bank tables are sorted by `code` with JavaScript's default sort,
 * which compares UTF-16 code units; every code is ASCII, so byte order is the
 * same order and a binary search over them is exact. */
static int cmp_nul_span(const char *a, const char *b, size_t blen)
{
    size_t alen = strlen(a);
    size_t n = alen < blen ? alen : blen;
    int c = n ? memcmp(a, b, n) : 0;
    if (c) return c;
    return alen < blen ? -1 : (alen > blen ? 1 : 0);
}

static int span_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    return alen == blen && (alen == 0 || memcmp(a, b, alen) == 0);
}

static int span_eq_lit(const char *a, size_t alen, const char *lit)
{
    return span_eq(a, alen, lit, strlen(lit));
}

static int starts_upper(const char *k, size_t klen)
{
    return klen > 0 && k[0] >= 'A' && k[0] <= 'Z';
}

/* Which of the 19 synthesis parameters this name is, or -1. An extras key
 * that names one is not an extra at all: it overwrites that parameter in the
 * event. Only the ten uppercase-initial names are reachable this way, because
 * an extras key is only ever created by the /^[A-Z]/ branch in
 * apply_directive(). */
static int param_index(const char *k, size_t klen)
{
    int i;
    for (i = 0; i < KL_PARAM_COUNT; i++)
        if (span_eq_lit(k, klen, kl_param_name[i])) return i;
    return -1;
}

static const kl_bank *bank_by_span(const char *n, size_t nlen)
{
    size_t i;
    for (i = 0; i < kl_bank_count; i++)
        if (span_eq_lit(n, nlen, kl_banks[i].name)) return &kl_banks[i];
    return NULL;
}

/* `phonemes[t.code]` in the JS is a plain object lookup, so it would also
 * find an Object.prototype member -- `constructor` is truthy and would render
 * as a target full of NaN rather than warn. Every phoneme code matches
 * /^[A-Z]+$/ and no member of Object.prototype is uppercase-only, so the path
 * is unreachable; the same defect in `part in PAUSE_MS` is written up in
 * docs/16-stage4-token.md. This searches the bank's own table only, which is
 * what the JS reaches. */
static const kl_phoneme *phoneme_by_span(const kl_bank *b,
                                         const char *c, size_t clen)
{
    size_t lo = 0, hi = b->phoneme_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp_nul_span(b->phonemes[mid].code, c, clen);
        if (r == 0) return &b->phonemes[mid];
        if (r < 0) lo = mid + 1;
        else       hi = mid;
    }
    return NULL;
}

/* --- section state -------------------------------------------------------- */

typedef struct {
    uint32_t present;
    double   v[KL_PARAM_COUNT];
} target;

static void tset(target *t, int i, double x)
{
    t->present |= (uint32_t)1u << i;
    t->v[i] = x;
}

typedef struct {
    const kl_token_list *T;
    const char *A;                 /* T->arena */
    kl_compile_arena *arena;
    kl_compiled *out;
    kl_voice *V;

    /* where this section's slice of each shared array begins */
    size_t ev_base, ph_base, wn_base;

    /* running voice state */
    double f0, rate, scale;
    double vibrato, vibrato_rate, tremolo, tremolo_rate;
    double aspiration, tilt, effort;

    /* the values a bare-letter reset returns to */
    double i_f0, i_rate, i_scale;
    double i_vibrato, i_vibrato_rate, i_tremolo, i_tremolo_rate;
    double i_aspiration, i_tilt, i_effort;

    const kl_bank *bank, *i_bank;

    const char *engine;   size_t engine_len;
    const char *i_engine; size_t i_engine_len;

    /* the live extras set, in insertion order */
    kl_extra *ex;
    size_t    n_ex;
    int       ex_dirty;
    kl_extra_span ex_span;

    double  time_ms;
    int32_t phrase_src_start;
    double  phrase_time_start;

    int in_syllable;
    size_t n_syl;

    int overflow;
} sect;

/* --- warnings ------------------------------------------------------------- */

static void warn_parts(sect *S, const char *lit, const char *tail, size_t tail_len)
{
    size_t lit_len = strlen(lit);
    size_t need = lit_len + tail_len;
    kl_compile_arena *a = S->arena;
    kl_warning *w;

    if (S->wn_base + S->V->n_warnings >= a->warnings_cap
        || S->out->text_len + need > a->text_cap) {
        S->overflow = 1;
        return;
    }
    memcpy(a->text + S->out->text_len, lit, lit_len);
    if (tail_len) memcpy(a->text + S->out->text_len + lit_len, tail, tail_len);

    w = &S->V->warnings[S->V->n_warnings++];
    w->text = a->text + S->out->text_len;
    w->len  = (uint32_t)need;
    S->out->text_len += need;
}

/* --- extras --------------------------------------------------------------- */

static void extras_set(sect *S, const char *k, size_t klen, double v)
{
    size_t i;
    for (i = 0; i < S->n_ex; i++) {
        if (span_eq(S->ex[i].key, S->ex[i].key_len, k, klen)) {
            S->ex[i].value = v;
            S->ex_dirty = 1;
            return;
        }
    }
    if (S->n_ex >= S->arena->live_cap) { S->overflow = 1; return; }
    S->ex[S->n_ex].key     = k;
    S->ex[S->n_ex].key_len = (uint32_t)klen;
    S->ex[S->n_ex].value   = v;
    S->n_ex++;
    S->ex_dirty = 1;
}

/* `delete extras[k]` on a key that is not there leaves the object alone, so a
 * miss must not mark the state dirty -- it would cost a pool snapshot that
 * kl_compile_need() did not budget for. */
static void extras_delete(sect *S, const char *k, size_t klen)
{
    size_t i;
    for (i = 0; i < S->n_ex; i++) {
        if (span_eq(S->ex[i].key, S->ex[i].key_len, k, klen)) {
            memmove(&S->ex[i], &S->ex[i + 1], (S->n_ex - i - 1) * sizeof S->ex[0]);
            S->n_ex--;
            S->ex_dirty = 1;
            return;
        }
    }
}

/* The digest sorts the extras keys, so the pool holds them sorted. Keys are
 * \w+ and therefore ASCII, so byte order is JavaScript's string order. */
static int extra_key_less(const kl_extra *a, const kl_extra *b)
{
    size_t n = a->key_len < b->key_len ? a->key_len : b->key_len;
    int c = n ? memcmp(a->key, b->key, n) : 0;
    if (c) return c < 0;
    return a->key_len < b->key_len;
}

/* Materialise the current extras state into the pool, skipping any whose key
 * names a synthesis parameter -- those were written into the event itself.
 * Events that share a state share a span, so the pool grows once per change
 * rather than once per event. */
static void extras_materialise(sect *S)
{
    kl_compile_arena *a = S->arena;
    size_t first = S->out->n_extras;
    size_t count = 0;
    size_t i, j;

    for (i = 0; i < S->n_ex; i++) {
        kl_extra e = S->ex[i];
        if (param_index(e.key, e.key_len) >= 0) continue;
        if (S->out->n_extras >= a->extras_cap) { S->overflow = 1; return; }
        j = count;
        while (j > 0 && extra_key_less(&e, &a->extras[first + j - 1])) {
            a->extras[first + j] = a->extras[first + j - 1];
            j--;
        }
        a->extras[first + j] = e;
        count++;
        S->out->n_extras++;
    }
    S->ex_span.first = (uint32_t)first;
    S->ex_span.count = (uint32_t)count;
    S->ex_dirty = 0;
}

/* --- emit ----------------------------------------------------------------- */

/* `{ ...target, ...extras, ...stateExtras() }`, in that order and for the
 * reason in the file header: each layer overwrites the last. */
static void emit(sect *S, target *t, double transition_ms)
{
    kl_compile_arena *a = S->arena;
    kl_event *e;
    size_t i;

    for (i = 0; i < S->n_ex; i++) {
        int pi = param_index(S->ex[i].key, S->ex[i].key_len);
        if (pi >= 0) tset(t, pi, S->ex[i].value);
    }

    tset(t, KL_VIBRATO_DEPTH, S->vibrato);
    tset(t, KL_VIBRATO_RATE,  S->vibrato_rate);
    tset(t, KL_TREMOLO_DEPTH, S->tremolo);
    tset(t, KL_TREMOLO_RATE,  S->tremolo_rate);
    tset(t, KL_ASPIRATION,    S->aspiration);
    tset(t, KL_TILT,          S->tilt);
    tset(t, KL_EFFORT,        S->effort);

    if (S->ex_dirty) extras_materialise(S);
    if (S->overflow) return;

    if (S->ev_base + S->V->n_events >= a->events_cap) { S->overflow = 1; return; }

    e = &S->V->events[S->V->n_events];
    e->at_ms         = S->time_ms;
    e->transition_ms = transition_ms;
    e->present       = t->present;
    memcpy(e->value, t->v, sizeof e->value);
    S->V->spans[S->V->n_events] = S->ex_span;
    S->V->n_events++;
}

static void emit_silence(sect *S, double transition_ms)
{
    target t;
    t.present = 0;
    memset(t.v, 0, sizeof t.v);
    tset(&t, KL_A1, 0.0);
    tset(&t, KL_A2, 0.0);
    tset(&t, KL_A3, 0.0);
    emit(S, &t, transition_ms);
}

/* `scaled()`. `g` non-NULL is the diphthong endpoint: it moves F1..F3 and
 * nothing else, because the only fields any bank's glideTo defines are those
 * three -- asserted by tools/build-banks-c.mjs, not assumed here. */
static void scaled(sect *S, const kl_phoneme *p, double f0_hz,
                   const kl_glide *g, target *t)
{
    t->present = 0;
    memset(t->v, 0, sizeof t->v);
    tset(t, KL_VOICING, p->voicing);
    tset(t, KL_FF1, (g ? g->F1 : p->F1) * S->scale);
    tset(t, KL_FF2, (g ? g->F2 : p->F2) * S->scale);
    tset(t, KL_FF3, (g ? g->F3 : p->F3) * S->scale);
    tset(t, KL_BW1, p->BW1 * S->scale);
    tset(t, KL_BW2, p->BW2 * S->scale);
    tset(t, KL_BW3, p->BW3 * S->scale);
    tset(t, KL_A1, p->A1);
    tset(t, KL_A2, p->A2);
    tset(t, KL_A3, p->A3);
    tset(t, KL_F0, f0_hz);
}

/* --- phrases -------------------------------------------------------------- */

static void emit_phrase(sect *S, const kl_token *t)
{
    kl_compile_arena *a = S->arena;
    kl_phrase *ph;

    if (S->ph_base + S->V->n_phrases >= a->phrases_cap) { S->overflow = 1; return; }

    ph = &S->V->phrases[S->V->n_phrases++];
    ph->src_start       = S->phrase_src_start;
    ph->src_end         = t->src_end;
    ph->token_src_start = t->src_start;
    ph->t_start_ms      = S->phrase_time_start;
    ph->t_end_ms        = S->time_ms;
    ph->kind            = t->type;
    if (t->type == KL_TOK_PHONEME) {
        ph->phoneme     = S->A + t->code_off;
        ph->phoneme_len = t->code_len;
    } else {
        ph->phoneme     = NULL;
        ph->phoneme_len = 0;
    }
    S->phrase_src_start  = t->src_end;
    S->phrase_time_start = S->time_ms;
}

/* --- rendering ------------------------------------------------------------ */

static void render_phoneme(sect *S, const kl_token *t, double slot_ms)
{
    const kl_phoneme *p = phoneme_by_span(S->bank, S->A + t->code_off, t->code_len);
    double start_f0, end_f0;
    target g;

    if (!p) {
        /* Time does not advance and nothing is emitted, but the caller still
         * records a phrase and still applies the pitch delta. */
        warn_parts(S, "unknown phoneme: ", S->A + t->code_off, t->code_len);
        return;
    }

    start_f0 = t->stressed ? S->f0 + KL_DEFAULT_STRESS_F0_LIFT : S->f0;
    end_f0   = start_f0 + t->pitch_delta;

    if (p->is_stop) {
        double burst_ms   = js_min(KL_DEFAULT_STOP_BURST_MS, slot_ms * 0.3);
        double silence_ms = slot_ms - burst_ms;
        emit_silence(S, js_min(20.0, silence_ms * 0.4));
        S->time_ms += silence_ms;
        scaled(S, p, start_f0, NULL, &g);
        emit(S, &g, js_min(5.0, burst_ms * 0.2));
        S->time_ms += burst_ms;
    } else if (p->has_glide) {
        double onset  = slot_ms * 0.25;
        double glide  = slot_ms * 0.50;
        double offset = slot_ms * 0.25;
        scaled(S, p, start_f0, NULL, &g);
        emit(S, &g, js_min(20.0, onset));
        S->time_ms += onset;
        scaled(S, p, end_f0, &p->glide_to, &g);
        emit(S, &g, glide);
        S->time_ms += glide + offset;
    } else if (t->pitch_delta != 0.0) {
        scaled(S, p, start_f0, NULL, &g);
        emit(S, &g, js_min(25.0, slot_ms * 0.25));
        S->time_ms += slot_ms * 0.25;
        scaled(S, p, end_f0, NULL, &g);
        emit(S, &g, slot_ms * 0.6);
        S->time_ms += slot_ms * 0.75;
    } else {
        double trans = js_min(KL_DEFAULT_TRANSITION_MS, slot_ms * 0.4);
        scaled(S, p, start_f0, NULL, &g);
        emit(S, &g, trans);
        S->time_ms += slot_ms;
    }
}

/* A group renders its phonemes into one slot of `rate`, divided evenly. Stress
 * still lifts F0 inside a group but no longer lengthens anything, because the
 * slot is fixed before the loop -- that asymmetry is the JS's, not a slip. */
static void flush_syllable(sect *S)
{
    double slot;
    size_t i;

    if (S->n_syl == 0) { S->in_syllable = 0; return; }
    slot = S->rate / (double)S->n_syl;
    for (i = 0; i < S->n_syl; i++) {
        const kl_token *t = S->arena->syl[i];
        render_phoneme(S, t, slot);
        emit_phrase(S, t);
        if (!t->transient) S->f0 += t->pitch_delta;
    }
    S->n_syl = 0;
    S->in_syllable = 0;
}

/* --- directives ----------------------------------------------------------- */

/* Reset to the section's initial value, add to the running value, or replace
 * it. Which of the three is decided by the tokenizer; this only applies it. */
static void apply_scalar(const kl_token *t, double *cur, double initial)
{
    if (t->reset)         *cur = initial;
    else if (t->relative) *cur += t->value;
    else                  *cur = t->value;
}

static void apply_directive(sect *S, const kl_token *t)
{
    const char *k = S->A + t->key_off;
    size_t klen = t->key_len;

    /* `base` and `pitch` are one state. Only `base` is reachable from the
     * compact form and the note form; `pitch` only from `[pitch=N]`. */
    if (span_eq_lit(k, klen, "base") || span_eq_lit(k, klen, "pitch")) {
        apply_scalar(t, &S->f0, S->i_f0);
    } else if (span_eq_lit(k, klen, "rate")) {
        apply_scalar(t, &S->rate, S->i_rate);
    } else if (span_eq_lit(k, klen, "scale")) {
        apply_scalar(t, &S->scale, S->i_scale);
    } else if (span_eq_lit(k, klen, "vibrato")) {
        apply_scalar(t, &S->vibrato, S->i_vibrato);
    } else if (span_eq_lit(k, klen, "vibratoRate")) {
        apply_scalar(t, &S->vibrato_rate, S->i_vibrato_rate);
    } else if (span_eq_lit(k, klen, "tremolo")) {
        apply_scalar(t, &S->tremolo, S->i_tremolo);
    } else if (span_eq_lit(k, klen, "tremoloRate")) {
        apply_scalar(t, &S->tremolo_rate, S->i_tremolo_rate);
    } else if (span_eq_lit(k, klen, "aspiration")) {
        apply_scalar(t, &S->aspiration, S->i_aspiration);
    } else if (span_eq_lit(k, klen, "tilt")) {
        apply_scalar(t, &S->tilt, S->i_tilt);
    } else if (span_eq_lit(k, klen, "effort")) {
        apply_scalar(t, &S->effort, S->i_effort);
    } else if (span_eq_lit(k, klen, "pause")) {
        /* The one directive that makes sound. It reads neither `reset` nor
         * `relative` and takes the absolute value, so `p-250` and `p250` are
         * the same pause. The JS is one unconditional
         * `timeMs += Math.abs(t.value)`, and the corpus pins it. */
        emit_silence(S, 30.0);
        S->time_ms += fabs(t->value);
        emit_phrase(S, t);
    } else if (starts_upper(k, klen)) {
        /* Extended, engine-specific state. klattsch does not interpret it; it
         * rides into every subsequent target for a consuming engine to read. */
        if (t->reset) extras_delete(S, k, klen);
        else          extras_set(S, k, klen, t->value);
    } else {
        warn_parts(S, "unknown directive: ", k, klen);
    }
}

/* --- one voice section ---------------------------------------------------- */

static double opt_or(const kl_compile_opts *o, kl_opt which, double dflt)
{
    if (o && (o->present & ((uint32_t)1u << (int)which))) return o->value[which];
    return dflt;
}

static int is_voice_marker(const kl_token_list *T, const kl_token *t)
{
    return t->type == KL_TOK_DIRECTIVE
        && span_eq_lit(T->arena + t->key_off, t->key_len, "voice");
}

/* Compile the tokens of section `want`, counting sections from 0 and skipping
 * the [voice=N] markers themselves. Walking the whole list once per section
 * costs nothing at these sizes and saves a scratch array. */
static int compile_section(const kl_token_list *T, size_t want,
                           const kl_compile_opts *opts, int32_t section_src_start,
                           kl_compile_arena *arena, kl_compiled *out, kl_voice *V,
                           size_t ev_base, size_t ph_base, size_t wn_base)
{
    sect S;
    size_t i, section = 0;

    memset(&S, 0, sizeof S);
    S.T = T; S.A = T->arena; S.arena = arena; S.out = out; S.V = V;
    S.ev_base = ev_base; S.ph_base = ph_base; S.wn_base = wn_base;

    V->events   = arena->events   + ev_base;
    V->spans    = arena->spans    + ev_base;
    V->phrases  = arena->phrases  + ph_base;
    V->warnings = arena->warnings + wn_base;
    V->n_events = V->n_phrases = V->n_warnings = 0;

    S.i_f0           = opt_or(opts, KL_OPT_BASE_F0,       KL_DEFAULT_BASE_F0);
    S.i_rate         = opt_or(opts, KL_OPT_RATE,          KL_DEFAULT_RATE);
    S.i_scale        = opt_or(opts, KL_OPT_SCALE,         1.0);
    S.i_vibrato      = opt_or(opts, KL_OPT_VIBRATO_DEPTH, 0.0);
    S.i_vibrato_rate = opt_or(opts, KL_OPT_VIBRATO_RATE,  5.0);
    S.i_tremolo      = opt_or(opts, KL_OPT_TREMOLO_DEPTH, 0.0);
    S.i_tremolo_rate = opt_or(opts, KL_OPT_TREMOLO_RATE,  5.0);
    S.i_aspiration   = opt_or(opts, KL_OPT_ASPIRATION,    0.0);
    S.i_tilt         = opt_or(opts, KL_OPT_TILT,          0.0);
    S.i_effort       = opt_or(opts, KL_OPT_EFFORT,        0.5);

    if (opts && opts->bank) {
        S.i_bank = bank_by_span(opts->bank, strlen(opts->bank));
        if (!S.i_bank) return KL_COMPILE_BAD_BANK;
    } else {
        S.i_bank = kl_bank_default();
    }

    S.f0           = S.i_f0;
    S.rate         = S.i_rate;
    S.scale        = S.i_scale;
    S.vibrato      = S.i_vibrato;
    S.vibrato_rate = S.i_vibrato_rate;
    S.tremolo      = S.i_tremolo;
    S.tremolo_rate = S.i_tremolo_rate;
    S.aspiration   = S.i_aspiration;
    S.tilt         = S.i_tilt;
    S.effort       = S.i_effort;
    S.bank         = S.i_bank;

    if (opts && opts->engine) {
        S.i_engine     = opts->engine;
        S.i_engine_len = strlen(opts->engine);
    }
    S.engine     = S.i_engine;
    S.engine_len = S.i_engine_len;

    S.ex = arena->live;
    if (opts) {
        for (i = 0; i < opts->n_extras; i++) {
            if (S.n_ex >= arena->live_cap) return KL_COMPILE_OVERFLOW;
            S.ex[S.n_ex].key     = opts->extras[i].key;
            S.ex[S.n_ex].key_len = (uint32_t)strlen(opts->extras[i].key);
            S.ex[S.n_ex].value   = opts->extras[i].value;
            S.n_ex++;
        }
    }
    S.ex_dirty = 1;

    S.phrase_src_start  = section_src_start;
    S.phrase_time_start = 0.0;

    for (i = 0; i < T->n_tokens && !S.overflow; i++) {
        const kl_token *t = &T->tokens[i];

        if (is_voice_marker(T, t)) { section++; continue; }
        if (section != want) continue;

        switch (t->type) {
        case KL_TOK_UNKNOWN:
            warn_parts(&S, "unknown token: ", S.A + t->text_off, t->text_len);
            break;

        case KL_TOK_BANK_SWITCH: {
            const kl_bank *b = bank_by_span(S.A + t->name_off, t->name_len);
            if (!b) warn_parts(&S, "unknown bank: ", S.A + t->name_off, t->name_len);
            else    S.bank = b;
            break;
        }
        case KL_TOK_BANK_RESET:
            S.bank = S.i_bank;
            break;

        case KL_TOK_ENGINE_SWITCH:
            S.engine     = S.A + t->name_off;
            S.engine_len = t->name_len;
            break;
        case KL_TOK_ENGINE_RESET:
            S.engine     = S.i_engine;
            S.engine_len = S.i_engine_len;
            break;

        case KL_TOK_SYLLABLE_OPEN:
            if (S.in_syllable) { warn_parts(&S, "nested ( ignored", NULL, 0); break; }
            S.in_syllable = 1;
            S.n_syl = 0;
            break;
        case KL_TOK_SYLLABLE_CLOSE:
            if (!S.in_syllable) { warn_parts(&S, "unmatched )", NULL, 0); break; }
            flush_syllable(&S);
            break;

        case KL_TOK_DIRECTIVE:
            apply_directive(&S, t);
            break;

        case KL_TOK_PAUSE:
            emit_silence(&S, 30.0);
            S.time_ms += t->ms;
            emit_phrase(&S, t);
            break;

        case KL_TOK_PHONEME:
            if (S.in_syllable) {
                if (S.n_syl >= arena->syl_cap) { S.overflow = 1; break; }
                arena->syl[S.n_syl++] = t;
            } else {
                double phone_rate = t->stressed
                    ? S.rate * KL_DEFAULT_STRESS_DURATION_FACTOR : S.rate;
                render_phoneme(&S, t, phone_rate);
                emit_phrase(&S, t);
                if (!t->transient) S.f0 += t->pitch_delta;
            }
            break;

        default:
            break;                 /* stress marks never reach the token list */
        }
    }

    if (S.in_syllable) {
        warn_parts(&S, "unclosed (", NULL, 0);
        flush_syllable(&S);
    }

    S.time_ms += KL_DEFAULT_SENTENCE_FINAL_HOLD_MS;
    emit_silence(&S, KL_DEFAULT_FADE_OUT_MS);
    S.time_ms += KL_DEFAULT_TRAIL_OFF_MS;

    /* Hold the final phrase highlighted to the end of the section. */
    if (V->n_phrases) V->phrases[V->n_phrases - 1].t_end_ms = S.time_ms;

    V->total_ms   = S.time_ms;
    V->engine     = S.engine;
    V->engine_len = S.engine_len;

    return S.overflow ? KL_COMPILE_OVERFLOW : KL_COMPILE_OK;
}

/* --- sizing --------------------------------------------------------------- */

static int is_extras_directive(const kl_token_list *T, const kl_token *t)
{
    if (t->type != KL_TOK_DIRECTIVE) return 0;
    /* Every named directive starts lowercase, so an uppercase key always
     * falls through to the extras branch. */
    return starts_upper(T->arena + t->key_off, t->key_len);
}

void kl_compile_need(const kl_token_list *toks, const kl_compile_opts *opts,
                     kl_compile_arena *need)
{
    size_t i;
    size_t n_voices = 1, n_phoneme = 0, n_pause = 0, n_extra_dir = 0;
    size_t n_opt_extras = (opts ? opts->n_extras : 0);

    memset(need, 0, sizeof *need);

    for (i = 0; i < toks->n_tokens; i++) {
        const kl_token *t = &toks->tokens[i];
        if (is_voice_marker(toks, t)) { n_voices++; continue; }
        if (t->type == KL_TOK_PHONEME) {
            n_phoneme++;
        } else if (t->type == KL_TOK_PAUSE) {
            n_pause++;
        } else if (t->type == KL_TOK_DIRECTIVE) {
            if (span_eq_lit(toks->arena + t->key_off, t->key_len, "pause")) n_pause++;
            if (is_extras_directive(toks, t)) n_extra_dir++;
        }
    }

    need->voices_cap = n_voices;
    /* At most two events per phoneme -- silence plus burst, onset plus glide,
     * or the two halves of a pitch move -- one per pause, and one fade-out
     * per voice. */
    need->events_cap   = 2 * n_phoneme + n_pause + n_voices;
    need->phrases_cap  = n_phoneme + n_pause;
    need->warnings_cap = toks->n_tokens + n_voices;
    /* Every warning is a fixed prefix plus at most one token's text, and no
     * token is warned about twice. */
    need->text_cap = toks->arena_len + 32 * need->warnings_cap + 64;
    /* The extras pool grows once per state change, not once per event: at
     * most (changes, plus one initial state per voice) snapshots of at most
     * (directives plus seeded) keys. Counting the tokens rather than assuming
     * a constant is what keeps this from being quadratic in the input. */
    need->extras_cap = (n_extra_dir + n_voices) * (n_extra_dir + n_opt_extras);
    need->live_cap   = n_extra_dir + n_opt_extras;
    need->syl_cap    = n_phoneme;
}

/* --- the whole compile ---------------------------------------------------- */

int kl_compile(const kl_token_list *T, const kl_compile_opts *opts,
               kl_compile_arena *arena, kl_compiled *out)
{
    size_t i, s;
    size_t n_voices = 1;
    size_t ev = 0, ph = 0, wn = 0;

    memset(out, 0, sizeof *out);
    out->voices     = arena->voices;
    out->extra_pool = arena->extras;
    out->text       = arena->text;

    /* Sections are positional: everything before the first [voice=N] is voice
     * 0. The markers are removed here, which is why compile_section never
     * sees a `voice` key and never warns about one. */
    for (i = 0; i < T->n_tokens; i++)
        if (is_voice_marker(T, &T->tokens[i])) n_voices++;
    if (n_voices > arena->voices_cap) return KL_COMPILE_OVERFLOW;

    for (s = 0; s < n_voices; s++) {
        int32_t start = 0;
        int rc;

        if (s > 0) {
            size_t seen = 0;
            for (i = 0; i < T->n_tokens; i++) {
                if (!is_voice_marker(T, &T->tokens[i])) continue;
                if (++seen == s) { start = T->tokens[i].src_end; break; }
            }
        }

        rc = compile_section(T, s, opts, start, arena, out, &out->voices[s],
                             ev, ph, wn);
        if (rc != KL_COMPILE_OK) return rc;

        ev += out->voices[s].n_events;
        ph += out->voices[s].n_phrases;
        wn += out->voices[s].n_warnings;

        /* totalMs is the maximum across sections, not voice 0's. */
        if (s == 0 || out->voices[s].total_ms > out->total_ms)
            out->total_ms = out->voices[s].total_ms;
    }

    out->n_voices   = n_voices;
    out->n_warnings = wn;
    return KL_COMPILE_OK;
}
