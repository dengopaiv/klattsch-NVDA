// Stage 0 of the C rewrite: capture the JavaScript engine's behaviour as
// goldens, before a line of C exists. See docs/REWRITE.md and docs/12-stage0.md.
//
//   node tools/goldens.mjs           capture (writes goldens/)
//   node tools/goldens.mjs --check   re-capture and diff against what is there
//
// The output must be byte-identical across runs and across machines, so
// nothing here may record a timestamp, a path, a duration or anything else
// that varies. Provenance that does vary lives in the --check output, not in
// the goldens.

import { createHash } from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync, rmSync, existsSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  tokenize, compile, compileString,
  FormantSynth, PARAMS, renderToBuffer,
  encodeWav, banks,
  glottalPulse, xorshift, softClip, BandpassBiquad,
} from '../src/engine/index.js';

const here = dirname(fileURLToPath(import.meta.url));
const outDir = join(here, '..', 'goldens');

// ---------------------------------------------------------------------------
// Canonical digests
//
// A digest is SHA-256 over a byte encoding a C implementation can reproduce
// exactly. Doubles go in as 8 little-endian IEEE-754 bytes -- never as text --
// so the digest is insensitive to how either language formats a float.
// ---------------------------------------------------------------------------

class Digest {
  constructor() { this.h = createHash('sha256'); this.b = Buffer.alloc(8); }
  f64(x) { this.b.writeDoubleLE(x, 0); this.h.update(this.b); return this; }
  u32(x) { const b = Buffer.alloc(4); b.writeUInt32LE(x >>> 0, 0); this.h.update(b); return this; }
  i32(x) { const b = Buffer.alloc(4); b.writeInt32LE(x | 0, 0); this.h.update(b); return this; }
  u8(x)  { this.h.update(Buffer.from([x & 0xff])); return this; }
  str(s) { const b = Buffer.from(s, 'utf8'); this.u32(b.length); this.h.update(b); return this; }
  bytes(b) { this.u32(b.length); this.h.update(Buffer.from(b)); return this; }
  hex() { return this.h.digest('hex'); }
}

// Fields that ride into a schedule target from the JS object spread but are
// not synthesis parameters: `scaled()` spreads the whole phoneme, so the
// phoneme's shape flags and its documentation come along. The C struct will
// not have them. This is the one documented difference between the two
// schedules, and it lives here, in one place, rather than as a tolerance
// scattered through the comparison.
//
// `ipa`, `example` and `source` were added in stage 5. They are per-phoneme
// documentation strings, present in both Japanese banks, and they reached the
// extras branch below -- where `d.f64(string)` does not throw but quietly
// digests a NaN. So five corpus cases were pinning the *positions* of two
// strings in a sorted key list and nothing else. Excluding them cannot mask a
// real directive: an extras key is only ever created by the `/^[A-Z]/` branch
// of the directive switch, so every extras key begins with an uppercase ASCII
// letter and these three cannot collide with one.
const NON_PARAM_TARGET_KEYS = new Set([
  'isStop', 'glideTo', 'voicing',
  'ipa', 'example', 'source',
]);

// `voicing` is in PARAMS, so it is digested; it is listed above only to
// document that it is deliberately *not* excluded. Remove it from the set.
NON_PARAM_TARGET_KEYS.delete('voicing');

function digestSchedule(schedule) {
  const d = new Digest();
  d.u32(schedule.length);
  for (const evt of schedule) {
    d.f64(evt.atMs ?? 0);
    d.f64(evt.transitionMs ?? 30);
    // The 19 synthesis parameters, in PARAMS order, each flagged present or not.
    for (const k of PARAMS) {
      if (k in evt.target) { d.u8(1); d.f64(evt.target[k]); }
      else d.u8(0);
    }
    // Engine-specific extras ([OQ=0.6] and friends), sorted for stability.
    const extras = Object.keys(evt.target)
      .filter((k) => !PARAMS.includes(k) && !NON_PARAM_TARGET_KEYS.has(k))
      .sort();
    d.u32(extras.length);
    for (const k of extras) { d.str(k); d.f64(evt.target[k]); }
  }
  return d.hex();
}

function digestTokens(tokens) {
  const d = new Digest();
  d.u32(tokens.length);
  for (const t of tokens) {
    d.str(t.type);
    d.str(t.code ?? '');
    d.str(t.key ?? '');
    d.str(t.name ?? '');
    d.str(t.text ?? '');
    d.u8(t.stressed ? 1 : 0);
    d.u8(t.transient ? 1 : 0);
    d.u8(t.relative ? 1 : 0);
    d.u8(t.reset ? 1 : 0);
    d.f64(t.value ?? 0);
    d.f64(t.pitchDelta ?? 0);
    d.f64(t.ms ?? 0);
    d.i32(t.srcStart ?? -1);
    d.i32(t.srcEnd ?? -1);
  }
  return d.hex();
}

function digestPhrases(phrases) {
  const d = new Digest();
  d.u32(phrases.length);
  for (const p of phrases) {
    d.i32(p.srcStart); d.i32(p.srcEnd); d.i32(p.tokenSrcStart);
    d.f64(p.tStartMs); d.f64(p.tEndMs);
    d.str(p.kind); d.str(p.phoneme ?? '');
  }
  return d.hex();
}

// Tier 2 lives or dies on these two. The float64 digest is the reference the C
// port is compared against at 1e-9; the int16 digest is the one that must match
// exactly, because that is what anybody actually hears.
function digestAudio(buf) {
  const f = new Digest();
  const q = new Digest();
  f.u32(buf.length);
  q.u32(buf.length);
  let peak = 0;
  let sumsq = 0;
  for (let i = 0; i < buf.length; i++) {
    const v = buf[i];
    f.f64(v);
    const a = v < 0 ? -v : v;
    if (a > peak) peak = a;
    sumsq += v * v;
    let s = v;
    if (s > 1) s = 1; else if (s < -1) s = -1;
    q.i32(Math.round(s * 32767));
  }
  return {
    samples: buf.length,
    peak,
    rms: buf.length ? Math.sqrt(sumsq / buf.length) : 0,
    f64: f.hex(),
    i16: q.hex(),
  };
}

// ---------------------------------------------------------------------------
// The corpus
//
// Adversarial about what a rewrite actually breaks, per docs/REWRITE.md. Every
// group below answers a specific way the port could go wrong; a group with no
// such reason does not belong here.
// ---------------------------------------------------------------------------

const VOWEL_FRAME = (p) => `HH AH ${p} AH`;

function buildCorpus() {
  const cases = [];
  const add = (group, id, text, opts) => cases.push({ group, id, text, ...(opts ? { opts } : {}) });

  // 1. Every phoneme in every bank, alone and in a vowel frame. Catches a
  //    mistranscribed table entry, which is otherwise invisible until someone
  //    hears the wrong vowel.
  for (const name of banks.list().sort()) {
    const bank = banks.get(name);
    const codes = Object.keys(bank.phonemes).filter((k) => !k.startsWith('_')).sort();
    for (const code of codes) {
      add('phoneme', `phoneme/${name}/${code}`, code, { bank: name });
      add('phoneme-frame', `phoneme-frame/${name}/${code}`, VOWEL_FRAME(code), { bank: name });
    }
  }

  // 2. Every directive, in all four forms: absolute bare, explicit =, relative
  //    +/-, and bare-letter reset. The reset form reads opts, so it is the one
  //    that breaks when initial state is mishandled.
  const letters = {
    b: [140, 90], r: [180, 60], s: [1.2, 0.8], v: [12, 3], w: [7, 4],
    m: [0.4, 0.1], n: [6, 3], h: [0.5, 0.2], t: [0.4, -0.4], g: [0.9, 0.2],
  };
  for (const [ltr, [a, b]] of Object.entries(letters)) {
    add('directive', `directive/${ltr}/abs`, `${ltr}${a} AA ${ltr}${b} AA`);
    add('directive', `directive/${ltr}/eq`, `${ltr}=${a} AA ${ltr}=${b} AA`);
    add('directive', `directive/${ltr}/rel`, `AA ${ltr}+${a} AA ${ltr}-${b} AA`);
    add('directive', `directive/${ltr}/reset`, `${ltr}${a} AA ${ltr} AA`);
  }
  // `p` has no bare form (the tokenizer drops it) and takes absolute values only.
  add('directive', 'directive/p/abs', 'AA p250 AA');
  add('directive', 'directive/p/bare-dropped', 'AA p AA');
  add('directive', 'directive/p/negative', 'AA p-250 AA');
  // `pitch` is the same state as `base`, and the only way to reach it is the
  // bracket form -- no compact letter maps to it. Without this the two can be
  // separated and nothing notices.
  add('directive', 'directive/pitch-bracket', '[pitch=200] AA AA');
  add('directive', 'directive/pitch-then-base', '[pitch=200] AA b+10 AA');

  // 3. Note names across the whole range, including the accidental forms and
  //    the negative octave. noteToHz is the compiler's only transcendental.
  const NOTES = ['C', 'D', 'E', 'F', 'G', 'A', 'B'];
  for (let oct = -1; oct <= 9; oct++) {
    for (const n of NOTES) {
      add('note', `note/${n}${oct}`, `b${n}${oct} AA`);
      add('note', `note/${n}s${oct}`, `b${n}#${oct} AA`);
      add('note', `note/${n}f${oct}`, `b${n}b${oct} AA`);
    }
  }
  add('note', 'note/eq-form', 'b=C4 AA');
  add('note', 'note/invalid', 'bH4 AA');

  // 4. Pitch deltas, sticky and transient. Sticky accumulates into the running
  //    f0 and transient does not -- a one-character difference in the source
  //    with a compounding effect on everything after it.
  add('pitch', 'pitch/sticky-up', 'AA+15 AA AA');
  add('pitch', 'pitch/sticky-down', 'AA-15 AA AA');
  add('pitch', 'pitch/transient-up', 'AA(+40) AA AA');
  add('pitch', 'pitch/transient-down', 'AA(-40) AA AA');
  add('pitch', 'pitch/mixed', 'AA+10 AA(+40) AA-10 AA');
  add('pitch', 'pitch/fractional', 'AA+7.5 AA(-2.25) AA');
  add('pitch', 'pitch/on-stop', 'P+20 AA');
  add('pitch', 'pitch/on-glide', 'AY+20 AA');
  add('pitch', 'pitch/stressed', "AA'+15 AA");

  // 5. Syllable groups, including every malformed form. Each has a defined
  //    behaviour and a warning string in the JS, and the warnings are part of
  //    the contract the C must reproduce.
  add('syllable', 'syllable/simple', '( HH AH ) ( L OW )');
  add('syllable', 'syllable/single', '( AA )');
  add('syllable', 'syllable/empty', '( )');
  add('syllable', 'syllable/many', '( HH AH L OW W ER L D )');
  add('syllable', 'syllable/nested', '( HH ( AH ) L )');
  add('syllable', 'syllable/unmatched-close', 'HH AH ) L OW');
  add('syllable', 'syllable/unclosed', '( HH AH L OW');
  add('syllable', 'syllable/close-only', ')');
  add('syllable', 'syllable/with-directive', '( b160 HH AH ) ( L OW )');
  add('syllable', 'syllable/with-pitch', '( HH+10 AH ) ( L OW )');

  // 6. Banks: switching mid-utterance, resetting, and naming one that is not
  //    there. The reset returns to the *opts* bank, not to the default.
  add('bank', 'bank/switch', 'AA [bank=ja-mokhtari-2000] A [bank] AA');
  add('bank', 'bank/reset-to-opts', 'AA [bank=ja-hecko-2026] A [bank] AA',
    { bank: 'ja-mokhtari-2000' });
  add('bank', 'bank/unknown', 'AA [bank=nope] AA');
  add('bank', 'bank/switch-twice', '[bank=ja-mokhtari-2000] A [bank=klatt1980-en] AA');
  // bank/reset-to-opts above cannot actually tell the opts bank from the
  // default one: it reads AA after the reset, and AA is inherited unchanged
  // from klatt1980-en by both Japanese banks, so all three answers agree. `A`
  // exists only in the Japanese banks and differs between them, so this is
  // the case that distinguishes the three.
  add('bank', 'bank/reset-to-opts-distinct', '[bank=ja-hecko-2026] A [bank] A',
    { bank: 'ja-mokhtari-2000' });

  // 7. Voices. Sections compile from a fresh initial state, so running
  //    directives must not carry across a marker -- the easiest thing in the
  //    whole compiler to get wrong.
  add('voice', 'voice/one', 'bC3 AA AA');
  add('voice', 'voice/two', 'bC3 AA r400 AA [voice=1] bC4 IY r400 IY');
  add('voice', 'voice/five', 'AA [voice=1] IY [voice=2] UW [voice=3] EH [voice=4] OW');
  add('voice', 'voice/empty-section', 'AA [voice=1] [voice=2] IY');
  add('voice', 'voice/leading-marker', '[voice=1] AA');
  add('voice', 'voice/no-carry', 'b200 r300 AA [voice=1] AA');
  add('voice', 'voice/uneven-lengths', 'AA AA AA AA [voice=1] IY');
  // Warnings are merged across sections in section order. Every other voice
  // case compiles cleanly, so voice 0's warnings could be reported as the
  // whole list and no golden would move.
  add('voice', 'voice/warning-in-second', 'AA [voice=1] ZZZ');
  add('voice', 'voice/warnings-in-both', 'ZZZ [voice=1] @@@ [voice=2] [qq=1]');

  // 8. Uppercase extras: set, overridden, cleared. They ride into every
  //    subsequent target as opaque state.
  add('extras', 'extras/set', '[OQ=0.65] AA AA');
  add('extras', 'extras/override', '[OQ=0.65] AA [OQ=0.3] AA');
  add('extras', 'extras/clear', '[OQ=0.65] AA [OQ] AA');
  add('extras', 'extras/multiple', '[OQ=0.65] [FNZ=450] AA [FNZ] AA');
  add('extras', 'extras/negative', '[TILT=-3.5] AA');
  add('extras', 'extras/from-opts', 'AA [OQ] AA', { extras: { OQ: 0.5 } });
  add('extras', 'extras/scaled-f4', '[F4=3300] [BW4=250] s1.2 AA');
  //     An extras key that names one of the 19 synthesis parameters is not an
  //     extra at all: the JS spread writes it straight over the phoneme's own
  //     value, and over a silence event's too. Ten of the nineteen are
  //     reachable this way -- the ones whose names begin with an uppercase
  //     letter, which is the only shape an extras key can have. Stage 5's
  //     mutation suite found the compiler could stop honouring them entirely
  //     and no golden moved.
  add('extras', 'extras/names-a-parameter', '[F1=900] AA');
  add('extras', 'extras/names-f0', '[F0=999] AA AA');
  add('extras', 'extras/names-a-parameter-on-silence', '[BW2=55] ,');

  // 9. Unknown input. The warning strings are the contract.
  add('unknown', 'unknown/phoneme', 'AA ZZZ AA');
  add('unknown', 'unknown/token', 'AA @@@ AA');
  add('unknown', 'unknown/directive', 'AA [qq=3] AA');
  add('unknown', 'unknown/lowercase-bracket', 'AA [oq=3] AA');
  add('unknown', 'unknown/several', 'ZZZ @@@ [qq=1]');
  // The directive key class is \w, which includes the underscore.
  add('unknown', 'unknown/underscore-key', 'AA [q_q=3] AA');

  // 10. Comments in every position a comment can occur, including the one that
  //     splits a token in half -- which the tokenizer handles by continuing to
  //     accumulate the token across the comment.
  add('comment', 'comment/line-start', '# a comment\nAA');
  add('comment', 'comment/line-mid', 'AA # trailing\nAA');
  add('comment', 'comment/hash-not-at-boundary', 'AA#notacomment AA');
  add('comment', 'comment/block', 'AA /* hidden */ AA');
  add('comment', 'comment/block-splitting-token', 'A/* split */A');
  add('comment', 'comment/block-unterminated', 'AA /* never closed');
  add('comment', 'comment/block-multiline', 'AA /* one\ntwo */ AA');
  add('comment', 'comment/only', '# nothing but a comment');
  // The `#` boundary test is only reachable with a non-space behind it
  // straight after a block comment; everywhere else the tokenizer has just
  // skipped whitespace. Without this case the test can be deleted and no
  // golden moves -- stage 4's mutation suite found exactly that.
  add('comment', 'comment/block-then-hash', 'AA /*x*/#tail AA');

  // 11. Normalization: NFKC, zero-width removal, and the homoglyph table.
  //     Cyrillic А and Greek Α both look like Latin A and must fold to it.
  add('normalize', 'normalize/cyrillic', 'АА');          // CYRILLIC А А
  add('normalize', 'normalize/greek', 'ΑΑ');             // GREEK ALPHA
  add('normalize', 'normalize/mixed-homoglyph', 'АA');
  add('normalize', 'normalize/zero-width', 'A​A');
  add('normalize', 'normalize/zwj', 'A‍A');
  add('normalize', 'normalize/bom', '﻿AA');
  add('normalize', 'normalize/nfkc-fullwidth', 'ＡＡ');    // FULLWIDTH A A
  add('normalize', 'normalize/lowercase-homoglyph', 'а');     // CYRILLIC а

  // 12. Rate, chosen either side of every min() cap in renderPhoneme. At a low
  //     rate the caps bind and the shape is cap-dominated; at a high rate they
  //     do not and it is fraction-dominated. Both paths need covering.
  //
  //     Rates 1 and 2 are here for one reason found in stage 3: they are the
  //     only ones that drive a transition below a single sample at 8 kHz, so
  //     they are the only ones that make the `Math.max(1, ...)` floor in the
  //     schedule conversion bind. At rate 10, the corpus's previous minimum,
  //     the shortest transition is still 4.8 samples.
  for (const r of [1, 2, 5, 10, 20, 40, 50, 60, 80, 110, 200, 400, 1000]) {
    add('rate', `rate/${r}/steady`, `r${r} AA AA`);
    add('rate', `rate/${r}/stop`, `r${r} P AA`);
    add('rate', `rate/${r}/glide`, `r${r} AY AA`);
    add('rate', `rate/${r}/pitch-move`, `r${r} AA+20 AA`);
    add('rate', `rate/${r}/stressed`, `r${r} AA' AA`);
  }

  // 13. Stress, both marks, and the retroactive application to the preceding
  //     phoneme token.
  add('stress', 'stress/apostrophe', "AA' AA");
  add('stress', 'stress/bang', 'AA! AA');
  add('stress', 'stress/separate-mark', "AA ' AA");
  add('stress', 'stress/mark-before-any', "' AA");
  add('stress', 'stress/mark-after-directive', "AA b150 ' AA");
  add('stress', 'stress/double', "AA' ' AA");
  // Two phonemes before the mark, so that searching the token list
  // forwards instead of backwards gives a different answer.
  add('stress', 'stress/mark-after-two', "AA BB ' CC");

  // 14. Pauses and the sentence-final path.
  add('pause', 'pause/comma', 'AA , AA');
  add('pause', 'pause/semicolon', 'AA ; AA');
  add('pause', 'pause/period', 'AA . AA');
  add('pause', 'pause/all', 'AA , AA ; AA . AA');
  add('pause', 'pause/leading', '. AA');
  add('pause', 'pause/only', '.');

  // 15. Engine markers, which the compiler records and does not interpret.
  add('engine', 'engine/set', '[engine=foo] AA');
  add('engine', 'engine/reset', '[engine=foo] AA [engine] AA');
  add('engine', 'engine/from-opts', '[engine=foo] AA [engine] AA', { engine: 'base' });
  add('engine', 'engine/per-voice', '[engine=a] AA [voice=1] [engine=b] IY');

  // 16. Degenerate input.
  add('edge', 'edge/empty', '');
  add('edge', 'edge/whitespace', '   \t\n  ');
  add('edge', 'edge/single-space', ' ');
  add('edge', 'edge/directive-only', 'b200');
  add('edge', 'edge/pause-only', ',');

  // 17. Whole utterances, the ones a person would actually type. These are the
  //     Tier 2 audio corpus, so they are longer than the unit cases above.
  add('utterance', 'utterance/hello', 'HH AH L OW');
  add('utterance', 'utterance/pangram',
    'DH AH K W IH K B R AW N F AA K S JH AH M P S OW V ER DH AH L EY Z IY D AO G .');
  add('utterance', 'utterance/sung', 'r200 bC#4 ( HH AH ) ( L OW )');
  add('utterance', 'utterance/contour', 'AY+15 D IH D');
  add('utterance', 'utterance/ornament', 'D IH D DH AE(+40) T');
  add('utterance', 'utterance/chord', 'bC3 AA r400 AA AA [voice=1] bC4 IY r400 IY IY');
  add('utterance', 'utterance/all-vowels', 'IY IH EH AE AA AO AH UH UW ER');
  add('utterance', 'utterance/all-stops', 'P B T D K G');
  add('utterance', 'utterance/all-fricatives', 'F TH S SH V DH Z ZH HH');
  add('utterance', 'utterance/all-nasals', 'M N NG');
  add('utterance', 'utterance/all-glides', 'W Y R L');
  add('utterance', 'utterance/all-diphthongs', 'AY AW EY OW OY');
  add('utterance', 'utterance/japanese', '[bank=ja-mokhtari-2000] A I U E O');

  // 18. Voice quality, rendered. Every one of these sets a parameter that the
  //     sample loop multiplies a mix constant by. Without them `aspiration`
  //     and `tremoloDepth` are zero in every rendered case, the constants
  //     multiply out, and a mutation to any of them is invisible -- which is
  //     exactly what the stage 0 mutation run found before these existed.
  add('voice-quality', 'vq/aspiration-low', 'h0.3 AA AA');
  add('voice-quality', 'vq/aspiration-high', 'h0.9 AA AA');
  add('voice-quality', 'vq/aspiration-full', 'h1 AA AA');
  add('voice-quality', 'vq/tremolo', 'm0.6 n5 AA AA');
  add('voice-quality', 'vq/tremolo-full', 'm1 n8 AA AA');
  add('voice-quality', 'vq/tremolo-slow', 'm0.5 n0.5 AA AA');
  add('voice-quality', 'vq/vibrato', 'v20 w6 AA AA');
  add('voice-quality', 'vq/vibrato-deep', 'v60 w3 AA AA');
  add('voice-quality', 'vq/tilt-positive', 't0.6 AA AA');
  add('voice-quality', 'vq/tilt-negative', 't-0.6 AA AA');
  add('voice-quality', 'vq/tilt-extreme', 't0.95 AA AA');
  add('voice-quality', 'vq/effort-lax', 'g0 AA AA');
  add('voice-quality', 'vq/effort-tense', 'g1 AA AA');
  add('voice-quality', 'vq/unvoiced', 'S F TH SH');
  add('voice-quality', 'vq/voiced-fricative', 'V DH Z ZH');
  add('voice-quality', 'vq/aspiration-on-unvoiced', 'h0.7 S F');
  add('voice-quality', 'vq/gain-low', 'AA', { gain: 0.5 });
  add('voice-quality', 'vq/all', 'h0.5 m0.4 n5 v15 w6 t0.3 g0.8 AA AA');
  //     Vibrato deeper than the base pitch, so the effective F0 goes negative
  //     and the glottal phase runs backwards. Found in stage 3: it is the only
  //     way to tell `floor` from truncation in the phase wrap, and nothing
  //     else in the corpus reaches it.
  add('voice-quality', 'vq/vibrato-exceeds-f0', 'b80 v200 w5 AA AA');
  add('voice-quality', 'vq/vibrato-far-exceeds-f0', 'b60 v400 w3 AA');

  // 19. Initial state supplied by the caller. Every `opts.x ?? default` in
  //     compileSection, and every bare-letter reset that reads it back.
  //
  //     Until stage 5 no corpus case set a single scalar option -- the 262
  //     that carried opts all carried `bank`, `extras`, `engine` or `gain` --
  //     so ten of the compiler's initial values could be taken from the
  //     defaults instead of from the caller and nothing moved. A screen
  //     reader sets rate and pitch on every utterance, so this is the path
  //     the shipped engine will spend its life in.
  const ALL_OPTS = {
    baseF0: 200, rate: 250, scale: 1.4,
    vibratoDepth: 9, vibratoRate: 9,
    tremoloDepth: 0.9, tremoloRate: 9,
    aspiration: 0.9, tilt: 0.5, effort: 0.9,
  };
  add('opts', 'opts/none', 'AA AA');
  add('opts', 'opts/all', 'AA AA', ALL_OPTS);
  add('opts', 'opts/reset-rate', 'r180 AA r AA', { rate: 250 });
  add('opts', 'opts/reset-base', 'b180 AA b AA', { baseF0: 200 });
  add('opts', 'opts/reset-scale', 's0.7 AA s AA', { scale: 1.4 });
  // Every bare-letter reset in one case, so a single initial value taken from
  // the wrong place cannot hide behind the other nine.
  add('opts', 'opts/reset-all',
    'b150 r150 s0.6 v3 w3 m0.3 n3 h0.3 t0.2 g0.3 AA b r s v w m n h t g AA',
    ALL_OPTS);
  add('opts', 'opts/relative-from-opts', 'AA b+10 r-50 AA', ALL_OPTS);

  return cases;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

// Audio is rendered for these groups only. Rendering every phoneme case at
// 48 kHz would add tens of megabytes of digest for no extra coverage -- the
// schedule already distinguishes them, and Tier 2 is about the sample loop.
const AUDIO_GROUPS = new Set([
  'utterance', 'rate', 'voice', 'pitch', 'syllable', 'voice-quality',
]);

// Sample rates worth capturing. 8000 matters because the biquad clamps to
// 0.45*sr, so a low rate is the only thing that exercises the upper clamp on
// F3 -- at 48 kHz it never binds.
const AUDIO_RATES = [48000, 22050, 8000];

function captureCase(c) {
  const parsed = tokenize(c.text);
  const compiled = compile(parsed, c.opts ?? {});

  const entry = {
    id: c.id,
    group: c.group,
    text: c.text,
    ...(c.opts ? { opts: c.opts } : {}),
    tokens: {
      count: parsed.tokens.length,
      digest: digestTokens(parsed.tokens),
      types: parsed.tokens.map((t) => t.type),
    },
    compile: {
      totalMs: compiled.totalMs,
      warnings: compiled.warnings,
      engine: compiled.engine ?? null,
      voiceCount: compiled.voices.length,
      events: compiled.schedule.length,
      scheduleDigest: digestSchedule(compiled.schedule),
      phrasesDigest: digestPhrases(compiled.phrases),
      voices: compiled.voices.map((v) => ({
        events: v.schedule.length,
        totalMs: v.totalMs,
        engine: v.engine ?? null,
        scheduleDigest: digestSchedule(v.schedule),
      })),
    },
  };

  if (AUDIO_GROUPS.has(c.group) && compiled.totalMs > 0) {
    entry.audio = {};
    for (const sr of AUDIO_RATES) {
      const buf = renderToBuffer({
        sampleRate: sr,
        schedule: compiled.schedule,
        totalMs: compiled.totalMs,
      });
      entry.audio[sr] = digestAudio(buf);
    }
  }

  return { entry, compiled, parsed };
}

// The primitives, tested directly rather than through an utterance, so a
// failure names the function instead of the sound.
function capturePrimitives() {
  // xorshift: the full period is 2^32-1; a million states is enough to catch a
  // wrong shift or a signed/unsigned slip, and cheap.
  const lfsr = new Digest();
  let state = 0xACE1ACE1 | 0;
  const firstStates = [];
  const firstSamples = [];
  for (let i = 0; i < 1_000_000; i++) {
    state = xorshift(state);
    lfsr.i32(state);
    if (i < 16) { firstStates.push(state); firstSamples.push(state / 2147483648); }
  }

  // glottalPulse across phase x effort. The two-lobe boundaries at Tp and
  // Tp+Tn move with effort, so the grid has to be fine enough to land on both
  // sides of each.
  const pulse = new Digest();
  for (let e = 0; e <= 100; e++) {
    for (let p = 0; p < 1000; p++) {
      pulse.f64(glottalPulse(p / 1000, e / 100));
    }
  }
  // Out-of-range effort is clamped; prove it.
  const pulseClamp = [
    glottalPulse(0.1, -1), glottalPulse(0.1, 0),
    glottalPulse(0.1, 1), glottalPulse(0.1, 2),
  ];

  // Biquad coefficients across the (f, bw, sr) grid, including the clamp
  // regions at both ends.
  const biq = new Digest();
  const sampleRates = [8000, 22050, 44100, 48000];
  for (const sr of sampleRates) {
    for (const f of [0, 10, 39, 40, 41, 100, 500, 1500, 2500, 3500, sr * 0.44, sr * 0.45, sr * 0.46, sr]) {
      for (const bw of [0, 10, 19, 20, 21, 50, 100, 200, 400, 1000]) {
        const b = new BandpassBiquad();
        b.setFreq(f, bw, sr);
        biq.f64(b.b0).f64(b.b1).f64(b.b2).f64(b.a1).f64(b.a2);
      }
    }
  }

  // The coefficient cache keys on the raw (f, bw) *before* clamping, so two
  // different raw values that clamp to the same thing still recompute. Record
  // it: a C port that clamps before caching is observably different.
  const cacheProbe = (() => {
    const b = new BandpassBiquad();
    b.setFreq(10, 5, 48000);          // clamps to (40, 20)
    const first = [b.b0, b.a1, b.a2];
    b.setFreq(20, 8, 48000);          // also clamps to (40, 20), but recomputes
    const second = [b.b0, b.a1, b.a2];
    b.setFreq(20, 8, 48000);          // same raw values: cache hit, no change
    const third = [b.b0, b.a1, b.a2];
    return { first, second, third, equalAfterClamp: first.every((v, i) => v === second[i]) };
  })();

  // softClip either side of the knee.
  const clip = new Digest();
  for (let i = -3000; i <= 3000; i++) clip.f64(softClip(i / 1000));

  return {
    xorshift: { states: 1_000_000, seed: '0xACE1ACE1', digest: lfsr.hex(), firstStates, firstSamples },
    glottalPulse: { gridPoints: 101 * 1000, digest: pulse.hex(), clamp: pulseClamp },
    biquad: { digest: biq.hex(), cacheProbe },
    softClip: { gridPoints: 6001, digest: clip.hex() },
  };
}

// noteToHz is the compiler's only transcendental, and the one declared
// exception to Tier 1 exactness (compared at 1e-9 Hz rather than bit for bit).
// Captured separately so that exception has one obvious home.
function captureNotes() {
  const out = {};
  for (let oct = -1; oct <= 9; oct++) {
    for (const n of ['C', 'D', 'E', 'F', 'G', 'A', 'B']) {
      for (const acc of ['', '#', 'b']) {
        const { schedule } = compileString(`b${n}${acc}${oct} AA`);
        if (schedule.length) out[`${n}${acc}${oct}`] = schedule[0].target.F0;
      }
    }
  }
  return out;
}

// The WAV encoder: header bytes, normalization gain, and the ICMT round-trip
// that lets a rendered file say how to regenerate itself.
function captureWav() {
  const { schedule, totalMs } = compileString('HH AH L OW');
  const buf = renderToBuffer({ sampleRate: 22050, schedule, totalMs });
  const plain = encodeWav(buf, 22050);
  const withMeta = encodeWav(buf, 22050, {
    metadata: { software: 'klattsch-goldens', comment: 'HH AH L OW' },
  });
  const noNorm = encodeWav(buf, 22050, { peakNormalize: 0 });
  // Three samples past full scale, normalization off: the only way to reach
  // the clamp. With peakNormalize on, the loudest sample is 0.95 by
  // construction and `if (s > 1)` can never bind -- five corpus cases mix
  // past 1.0 and still do not reach it.
  const loud = Float32Array.from(buf, (v) => v * 3);
  const clipped = encodeWav(loud, 22050, { peakNormalize: 0 });
  // A zero-length buffer: a 44-byte file, and the one thing that reaches the
  // `if (peak > 0)` guard by way of a peak that is exactly zero.
  const empty = encodeWav(new Float32Array(0), 22050);
  // An empty string is falsy in JavaScript, so a present-but-empty software
  // field is an absent one and the file gets ICMT without ISFT. "Present and
  // empty" and "absent" have to stay the same thing in the C.
  const emptySoftware = encodeWav(buf, 22050, { metadata: { software: '', comment: 'x' } });
  const hash = (b) => createHash('sha256').update(b).digest('hex');
  return {
    plain: { bytes: plain.bytes.length, gain: plain.gain, header: [...plain.bytes.slice(0, 44)], digest: hash(plain.bytes) },
    withMeta: { bytes: withMeta.bytes.length, gain: withMeta.gain, digest: hash(withMeta.bytes),
      infoChunk: [...withMeta.bytes.slice(44 + (plain.bytes.length - 44))] },
    noNormalize: { bytes: noNorm.bytes.length, gain: noNorm.gain, digest: hash(noNorm.bytes) },
    // An odd-length comment forces the pad byte that keeps chunks word-aligned.
    oddComment: (() => {
      const w = encodeWav(buf, 22050, { metadata: { comment: 'odd' } });
      return { bytes: w.bytes.length, digest: hash(w.bytes) };
    })(),
    clipped: { bytes: clipped.bytes.length, gain: clipped.gain, digest: hash(clipped.bytes) },
    empty: { bytes: empty.bytes.length, gain: empty.gain, digest: hash(empty.bytes),
      header: [...empty.bytes] },
    emptySoftware: { bytes: emptySoftware.bytes.length, gain: emptySoftware.gain,
      digest: hash(emptySoftware.bytes) },
  };
}

// Stable JSON: keys in insertion order is not enough, because a future edit
// could reorder them. Sort every object key, everywhere.
function stableStringify(value) {
  return JSON.stringify(value, (k, v) => {
    if (v && typeof v === 'object' && !Array.isArray(v)) {
      return Object.fromEntries(Object.keys(v).sort().map((key) => [key, v[key]]));
    }
    return v;
  }, 2) + '\n';
}

function capture() {
  const corpus = buildCorpus();
  const entries = [];
  const schedules = {};
  for (const c of corpus) {
    const { entry, compiled } = captureCase(c);
    entries.push(entry);
    // Stage 3 drives the C sample loop from a golden schedule, before the C
    // compiler exists. Keep the full schedules for the cases that get audio.
    if (AUDIO_GROUPS.has(c.group)) {
      schedules[c.id] = compiled.voices.map((v) => v.schedule);
    }
  }

  const byGroup = {};
  for (const e of entries) byGroup[e.group] = (byGroup[e.group] ?? 0) + 1;

  return {
    'manifest.json': stableStringify({
      schemaVersion: 1,
      engine: JSON.parse(readFileSync(join(here, '..', 'package.json'), 'utf8')).version,
      banks: banks.list().sort(),
      params: PARAMS,
      audioGroups: [...AUDIO_GROUPS].sort(),
      audioRates: AUDIO_RATES,
      excludedTargetKeys: [...NON_PARAM_TARGET_KEYS].sort(),
      counts: { cases: entries.length, byGroup },
    }),
    'cases.json': stableStringify(entries),
    'primitives.json': stableStringify(capturePrimitives()),
    'notes.json': stableStringify(captureNotes()),
    'wav.json': stableStringify(captureWav()),
    'schedules.json': stableStringify(schedules),
  };
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

const files = capture();
const check = process.argv.includes('--check');

if (check) {
  let bad = 0;
  const existing = existsSync(outDir) ? readdirSync(outDir).filter((f) => f.endsWith('.json')).sort() : [];
  const expected = Object.keys(files).sort();
  if (existing.join(',') !== expected.join(',')) {
    process.stderr.write(`goldens: file set differs\n  have: ${existing.join(', ')}\n  want: ${expected.join(', ')}\n`);
    bad++;
  }
  for (const [name, content] of Object.entries(files)) {
    const path = join(outDir, name);
    if (!existsSync(path)) { process.stderr.write(`goldens: ${name} missing\n`); bad++; continue; }
    // Compare content, not line endings -- same reasoning as build-banks.js.
    const have = readFileSync(path, 'utf8').replace(/\r\n/g, '\n');
    if (have !== content) {
      process.stderr.write(`goldens: ${name} DIFFERS from a fresh capture\n`);
      bad++;
    }
  }
  if (bad) {
    process.stderr.write('\nGoldens do not match the engine. If the engine changed deliberately,\n'
      + 're-run `node tools/goldens.mjs` and review the diff before committing it.\n');
    process.exit(1);
  }
  const m = JSON.parse(files['manifest.json']);
  process.stdout.write(`goldens up to date (${m.counts.cases} cases, ${Object.keys(files).length} files)\n`);
} else {
  // Remove the JSON goldens this script owns, and only those. Wiping the
  // whole directory also deleted the derived binaries beside them --
  // schedules.bin, which stage 3 reads, and cases-text.bin, numbers.bin and
  // divergences.bin, which stage 4 does. Re-capturing the goldens would then
  // break the next stage run with a missing-file error that said nothing
  // about the cause. Found by stage 4's mutation suite, which re-captured and
  // then could not find its own input.
  mkdirSync(outDir, { recursive: true });
  for (const f of readdirSync(outDir)) {
    if (f.endsWith('.json')) rmSync(join(outDir, f), { force: true });
  }
  for (const [name, content] of Object.entries(files)) {
    writeFileSync(join(outDir, name), content);
  }
  const m = JSON.parse(files['manifest.json']);
  process.stdout.write(`wrote goldens/ -- ${m.counts.cases} cases\n`);
  for (const [g, n] of Object.entries(m.counts.byGroup).sort()) {
    process.stdout.write(`  ${g.padEnd(16)} ${n}\n`);
  }
}
