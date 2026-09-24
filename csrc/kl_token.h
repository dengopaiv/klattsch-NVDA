/* kl_token.h -- the phoneme-string tokenizer, translated from `tokenize()`
 * and `classifyPart()` in src/engine/sequencer.js.
 *
 * klattsch is Tony Gies's work and is MIT licensed; see LICENSE. This file is
 * a translation of part of it and carries the same notice.
 *
 * Storage is the caller's, as everywhere else in this port: no malloc, so the
 * NVDA driver can tokenize on its synth thread without an allocator in the
 * path. kl_token_need() sizes the three buffers from the input length.
 *
 * Strings are held as UTF-8 in an arena and referred to by offset and length,
 * because the reference digests them as UTF-8 bytes and because a token's
 * `text` can be as long as the input.
 */
#ifndef KL_TOKEN_H
#define KL_TOKEN_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    KL_TOK_SYLLABLE_OPEN = 0,
    KL_TOK_SYLLABLE_CLOSE,
    KL_TOK_PAUSE,
    KL_TOK_STRESS_MARK,     /* never reaches the output; it marks a phoneme */
    KL_TOK_BANK_SWITCH,
    KL_TOK_BANK_RESET,
    KL_TOK_ENGINE_SWITCH,
    KL_TOK_ENGINE_RESET,
    KL_TOK_DIRECTIVE,
    KL_TOK_PHONEME,
    KL_TOK_UNKNOWN,
    KL_TOK_TYPE_COUNT
} kl_tok_type;

/* The reference's type strings, in enum order. The goldens digest the string,
 * not an ordinal, so these are part of the interface and not decoration. */
extern const char *const kl_tok_type_name[KL_TOK_TYPE_COUNT];

typedef struct {
    kl_tok_type type;
    uint32_t code_off, code_len;   /* phoneme code, e.g. "AA"            */
    uint32_t key_off,  key_len;    /* directive key, e.g. "base"         */
    uint32_t name_off, name_len;   /* bank or engine name                */
    uint32_t text_off, text_len;   /* verbatim text of an unknown token  */
    uint8_t  stressed, transient, relative, reset;
    double   value;                /* directive value                    */
    double   pitch_delta;          /* phoneme pitch offset               */
    double   ms;                   /* pause length                       */
    int32_t  src_start, src_end;   /* UTF-16 offsets into the normalized source */
} kl_token;

typedef struct {
    uint16_t *source;    size_t source_len, source_cap;
    kl_token *tokens;    size_t n_tokens,  tokens_cap;
    char     *arena;     size_t arena_len, arena_cap;
} kl_token_list;

/* Worst-case buffer sizes for an input of `len` UTF-8 bytes. Generous on
 * purpose: getting this wrong is a buffer overrun, and the amounts are small
 * next to a second of audio. */
void kl_token_need(size_t len, size_t *source_units, size_t *max_tokens, size_t *arena_bytes);

#define KL_TOKEN_OK         0
#define KL_TOKEN_OVERFLOW  (-1)

/* Tokenize UTF-8 input. The buffers in `out` must already point at storage
 * with the capacities kl_token_need() asks for; the three *_len/n_tokens
 * fields are set by this call. */
int kl_tokenize(const char *utf8, size_t len, kl_token_list *out);

/* Exposed for the stage 4 exit test: the reference parses directive values
 * with Number(), and this is the C's answer to it. See kl_token.c for the
 * range over which it is exact and docs/16-stage4-token.md for the grid that
 * was compared. */
double kl_parse_decimal(const uint16_t *s, size_t n);

#endif /* KL_TOKEN_H */
