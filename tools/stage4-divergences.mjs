// The places where csrc/kl_token.c deliberately does not do what
// src/engine/sequencer.js does, and the inputs that demonstrate each one.
//
// A divergence that lives only in a comment is a divergence nobody re-checks.
// These are executed by tools/verify-stage4.mjs on every run, in both
// directions: the reference must still misbehave, and the C must still do the
// sane thing. If upstream ever fixes one of these, this file fails and says
// so, which is the notification we want rather than a silent drift.
//
// docs/16-stage4-token.md carries the measurement behind each entry.

export const DIVERGENCES = [
  // `classifyPart` asks `part in PAUSE_MS`, and `in` walks the prototype
  // chain. Every own-property name of Object.prototype therefore classifies
  // as a pause whose `ms` is a function -- which downstream makes `atMs` a
  // string by concatenation and `totalMs` NaN, with `warnings` still empty.
  ...['toString', 'valueOf', 'constructor', 'hasOwnProperty',
      'isPrototypeOf', 'propertyIsEnumerable', 'toLocaleString', '__proto__']
    .map((text) => ({
      text,
      why: 'Object.prototype member reaches `part in PAUSE_MS`',
      jsType: 'pause',
      jsMsIsNotANumber: true,
      cType: 'unknown',
    })),
];

// Inputs that are *not* divergences but are worth pinning anyway, because
// each one is a behaviour a mechanical translation gets wrong and none of
// them is obvious from reading the code.
export const PINNED = [
  { text: 'A¨B', why: 'NFKC of U+00A8 is space + U+0308, so one token becomes two' },
  { text: 'A B', why: 'U+1680 is /\\s/ and NFKC leaves it alone' },
  { text: 'A B', why: 'U+2028 is /\\s/ and NFKC leaves it alone' },
  { text: 'A　B', why: 'U+3000 normalizes to a plain space' },
  { text: 'b=', why: 'the compact form needs digits; `b=` is unknown' },
  { text: 'b+', why: 'a sign with no digits is unknown' },
  { text: 'AA5', why: 'a phoneme delta needs a sign' },
  { text: 'bb4', why: 'not a note (lowercase) and not compact' },
  { text: 'bC4', why: 'a note without the equals sign' },
  { text: 'b=A-1', why: 'a negative octave' },
  { text: '[fnz]', why: 'a lowercase bracket is not a directive' },
  { text: '[FNZ]', why: 'an uppercase bracket resets an extended directive' },
  { text: '[bank=5]', why: 'the bank pattern wins over the generic key=value one' },
  { text: 'p', why: 'a bare `p` is dropped, producing no token at all' },
  { text: 'A#B', why: '`#` inside a token is not a comment' },
  { text: 'A /*x*/ B', why: 'a block comment between tokens' },
  { text: 'A/*x*/B', why: 'a block comment inside a token splices the halves' },
  { text: 'A/*unterminated', why: 'an unterminated block comment runs to the end' },
  { text: "AA' BB!", why: 'both stress marks, attached to the preceding phoneme' },
  { text: "! AA", why: 'a stress mark with no phoneme before it' },
];

export const ALL = [...DIVERGENCES.map((d) => d.text), ...PINNED.map((p) => p.text)];
