/* kl_norm.h -- input normalization, translated from the `normalize()` step of
 * src/engine/sequencer.js.
 *
 * klattsch is Tony Gies's work and is MIT licensed; see LICENSE. This file is
 * a translation of part of it and carries the same notice.
 *
 * The JavaScript does three passes over a UTF-16 string:
 *
 *     input.normalize('NFKC')
 *          .replace(ZERO_WIDTH_RE, '')
 *          .replace(HOMOGLYPH_RE, ch => HOMOGLYPH_MAP[ch] ?? ch)
 *
 * so this works in UTF-16 too, rather than UTF-8. That is not a stylistic
 * choice: the tokenizer records srcStart/srcEnd offsets into the normalized
 * string, those offsets are UTF-16 code-unit indices in the reference, and
 * they are part of what the stage 4 exit test compares. Working in UTF-8 and
 * converting would have to get the conversion exactly right at every token
 * boundary; working in UTF-16 has nothing to get right.
 *
 * NFKC here is the singleton table in kl_norm_data.c and nothing else -- no
 * canonical composition, no canonical reordering. That is bounded rather than
 * hoped: of the 12,236 code points with a multi-character NFD, none is ASCII
 * and none is whitespace, and no ASCII code point is moved by NFKC or NFD at
 * all. Composition therefore cannot produce a character this grammar reads.
 * The residue is that the `text` of an `unknown` token holding a combining
 * mark may differ from the reference's. docs/16-stage4-token.md has the
 * measurement and the case that demonstrates the residue.
 */
#ifndef KL_NORM_H
#define KL_NORM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t cp;    /* code point NFKC moves          */
    uint32_t off;   /* start in kl_norm_pool          */
    uint16_t len;   /* length in UTF-16 code units    */
} kl_norm_entry;

typedef struct {
    uint16_t from;  /* Greek or Cyrillic look-alike   */
    char     to;    /* the Latin letter it becomes    */
} kl_homoglyph;

extern const uint16_t      kl_norm_pool[];
extern const kl_norm_entry kl_norm_table[];
extern const size_t        kl_norm_table_len;
extern const uint16_t      kl_zero_width[];
extern const size_t        kl_zero_width_len;
extern const kl_homoglyph  kl_homoglyph_table[];
extern const size_t        kl_homoglyph_table_len;

/* True for exactly the code units JavaScript's /\s/ matches. Note U+FEFF is
 * one of them, which is why the zero-width strip runs before tokenizing and
 * not after: the reference strips it, so it never gets to be whitespace. */
int kl_is_space(uint16_t unit);

/* Decode UTF-8 into UTF-16 and apply the three passes. Returns the number of
 * UTF-16 code units written, or KL_NORM_OVERFLOW if out_cap is too small.
 *
 * Worst case growth is 18 UTF-16 units per input code point (U+FDFA), so
 * out_cap >= 18 * in_len always suffices; kl_norm_bound() says so in one
 * place rather than at every call site.
 *
 * Invalid UTF-8 becomes U+FFFD, one per maximal invalid subsequence. The
 * reference never meets invalid input -- a JavaScript string is already
 * well-formed UTF-16 -- so this is a decision the C has to make on its own,
 * and it is written down here because there is nothing to compare it against.
 */
#define KL_NORM_OVERFLOW ((size_t)-1)

size_t kl_norm_bound(size_t in_len);
size_t kl_normalize_utf8(const char *in, size_t in_len, uint16_t *out, size_t out_cap);

/* Encode a UTF-16 range back to UTF-8, as Buffer.from(s, 'utf8') does. Used
 * for the token text fields, which the goldens digest as UTF-8 bytes.
 * Returns bytes written, or KL_NORM_OVERFLOW. Unpaired surrogates become
 * U+FFFD, which is what Node's UTF-8 encoder does with them. */
size_t kl_utf16_to_utf8(const uint16_t *in, size_t in_len, char *out, size_t out_cap);

#endif /* KL_NORM_H */
