/* kl_text.h -- the English text front end: text in, klattsch source out.
 *
 * klattsch takes phoneme strings.  A screen reader hands over text.  This is
 * the piece between them, and its output is nothing more exotic than ordinary
 * klattsch source -- "b b+9.6 HH AH L OW' ." -- so everything it decides is
 * visible, diffable, and checked by the same tokenizer and compiler as a
 * string somebody typed.
 *
 * Four passes, per sentence:
 *
 *   1. Letter to sound: the NRL rules (kl_text_rules.c) with Votraxxion's
 *      matcher and number reader, lifted unchanged.  English spelling to
 *      ARPABET, numbers, money and the three abbreviations included.  Since
 *      2026-10-28 each word is first looked up in the compiled-in CMU
 *      Pronouncing Dictionary (kl_dict.h), which supplies its phones and its
 *      primary stress; the rules handle only the words it lacks.  Numbers are
 *      read before any lookup.  docs/23-dictionary.md.
 *   2. Symbols: the rules' mixed-case ARPABET split into symbols and mapped
 *      onto the klatt1980-en bank, which has no AX and no WH.
 *   3. Stress: one primary stress per content word, from spelling cues.
 *      klattsch marks it with `'`, which lengthens the phoneme by 1.5 and
 *      lifts its F0 by 8 Hz.
 *   4. Contour: a declining statement, a falling or rising end, a small rise
 *      before a comma, written as `b` directives and pitch deltas.
 *
 * Not a translation of klattsch -- upstream has no text front end.  Passes 1's
 * matcher and number reader come from Votraxxion's src/ttv.c (BSD-3-Clause);
 * passes 2 to 4 are new here.  See docs/19-frontend-text.md for how each was
 * measured, and NOTICE.md for the terms.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Päiv Dengo
 *
 * No allocation, like everything else on the speech path: every working
 * buffer lives in a kl_text_ctx the caller provides (about 135 KB -- static or
 * heap in a driver, not a thread stack).  Nothing is static and mutable, so
 * two contexts can run on two threads.
 */
#ifndef KL_TEXT_H
#define KL_TEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Working-buffer sizes, per sentence.  A sentence longer than KL_TEXT_MAX
 * bytes is truncated at a byte boundary before any rule runs -- NVDA hands
 * over a line or a sentence at a time, so this is a guard, not a limit
 * anybody should meet.  Output is the caller's buffer and has no limit here. */
#define KL_TEXT_MAX   4096   /* one sentence, upper-cased and padded       */
#define KL_TEXT_ARPA  8192   /* its ARPABET, before symbols (~2.4x)        */
#define KL_TEXT_ITEMS 4096   /* symbols, word breaks and pauses            */

typedef struct {
    char   sym[4];    /* klattsch symbol, NUL-terminated; empty for a break */
    int    kind;      /* KL_TEXT_ITEM_* in kl_text.c                         */
    int    src;       /* index into the working text of the rule behind it  */
    int    flags;     /* vowel / reduced / stressed / pitch                  */
    int    delta10;   /* pitch delta in tenths of a hertz, when flagged      */
} kl_text_item;

typedef struct {
    char         sent[KL_TEXT_MAX];      /* the sentence being gathered   */
    char         expanded[KL_TEXT_MAX];  /* ... with abbreviations out    */
    char         text[KL_TEXT_MAX];
    int          text_len;
    char         arpa[KL_TEXT_ARPA];
    int          arpa_src[KL_TEXT_ARPA];
    int          arpa_len;
    int          arpa_full;   /* what it would have been, unbounded */
    int          cur_src;
    kl_text_item items[KL_TEXT_ITEMS];
    int          item_count;
    int          use_dict;    /* set by every entry point; callers need not */
} kl_text_ctx;

/* Zero the whole struct before setting any field (memset, or `= {0}`): every
 * field's zero means "the default", and a field added later is read from
 * whatever the caller left there. */
typedef struct {
    /* The voice's base F0 in Hz.  Only the size of the contour depends on
     * it -- a rise is a fraction of the voice's pitch, not a fixed number of
     * hertz -- and the pitch itself stays the compiler's: the contour resets
     * with a bare `b`, which goes back to whatever base the compiler was
     * given.  0 means klattsch's default, 120. */
    double base_f0;

    /* The pause for a comma, in ms, written as klattsch's `p` directive.
     * The engine's own `,` token is 100 ms, a constant upstream froze; heard
     * through the generator on 2026-09-25 it was judged a bit short, so the
     * front end writes its own length instead and leaves the engine alone.
     * 0 means the front end's default, KL_TEXT_COMMA_MS. */
    int comma_ms;

    /* A scale on every size in the contour -- the start level, the
     * declination, the nucleus fall, the question rise and the continuation
     * rise -- which is what NVDA calls inflection.  1 is the contour as
     * written; 0 is flat, leaving only the `b` resets; 2 is twice as wide.
     * The engine's own 8 Hz stress lift is not part of the contour and is not
     * scaled.  Read only when `set_inflection` is nonzero, so that a zeroed
     * kl_text_opts keeps the contour as it is; a negative value is taken as
     * 0.  Added 2026-10-28 for the NVDA add-on (docs/NVDA-ADDON.md). */
    double inflection;
    int    set_inflection;

    /* Nonzero turns the dictionary off: every word goes to the rules, and the
     * output is byte for byte the front end of before the dictionary, which
     * goldens/text/source.json still pins.  Zero, the default, looks words up
     * first.  Added 2026-10-28 (docs/23-dictionary.md). */
    int    no_dictionary;
} kl_text_opts;

#define KL_TEXT_COMMA_MS 200

/* Text to klattsch source, with stress and a sentence contour.  UTF-8 in.
 * Writes at most `cap` bytes including the terminating NUL (when cap > 0),
 * cutting a long result at a token boundary rather than inside a token, and
 * returns the length the whole result has -- so a caller whose buffer was too
 * small can tell, and size the next one.  `opts` may be NULL. */
size_t kl_text_to_source(kl_text_ctx *x, const char *text,
                         const kl_text_opts *opts, char *out, size_t cap);

/* Read text out character by character, the way a screen reader reads a
 * password field or an unfamiliar word: every character by name, a comma
 * between them, no contour.  Characters outside ASCII are skipped. */
size_t kl_text_spell(kl_text_ctx *x, const char *text, char *out, size_t cap);

/* One word to its phonemes with stress and nothing else -- no contour, no
 * pauses.  What the stress measurement in tools/measure-text.mjs reads.
 * Rules only, never the dictionary: measured against CMU, a word the
 * dictionary supplied would score 100 % by construction. */
size_t kl_text_word(kl_text_ctx *x, const char *word, char *out, size_t cap);

/* Pass 1 alone: the rules' raw ARPABET for `text`, exactly as the matcher
 * produced it.  For comparing the lift with Votraxxion, and nothing else. */
size_t kl_text_nrl(kl_text_ctx *x, const char *text, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* KL_TEXT_H */
