// Stage 4 exit test: csrc/kl_token.c and csrc/kl_norm.c against the
// JavaScript tokenizer they came from. See docs/16-stage4-token.md.
//
//   node tools/verify-stage4.mjs <kl_token_dump> <kl_norm_dump>
//
// Back to Tier 1 after stage 3: no tolerance anywhere in this file. Every
// comparison is an exact equality, because every quantity is a string, an
// integer, or a double the grammar pins to a shape. The one arithmetic
// quantity is Number(), which gets its own grid.
//
// Three things are checked, in the order a failure is easiest to read in:
//
//   1. normalization, exhaustively -- all 1,112,064 code points,
//   2. Number(), over a grid of the shapes the grammar can produce,
//   3. the tokens themselves, field by field, over the whole corpus, and
//      their digest against the frozen goldens.
//
// (3) is compared in both directions on purpose: field-by-field against a
// live tokenize(), which says *what* differs, and by digest against
// goldens/cases.json, which ties the C to the frozen reference rather than to
// whatever the JavaScript happens to do today. Stage 1 learned that the first
// without the second is a comparison with no reference in it.

import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { tokenize } from '../src/engine/sequencer.js';
import { numberGrid } from './cases-to-bin.mjs';
import { DIVERGENCES, PINNED } from './stage4-divergences.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');

const tokenDump = process.argv[2];
const normDump = process.argv[3];
if (!tokenDump || !existsSync(tokenDump)) {
  process.stderr.write('usage: node tools/verify-stage4.mjs <kl_token_dump> [<kl_norm_dump>]\n');
  process.exit(2);
}

const fromDir = statSync(tokenDump).isDirectory();

// Either run the tool, or read what it wrote somewhere else. WSL's Debian
// has the only independent libm on this machine and no Node, so its dumps
// are captured there and compared here -- the arrangement stage 3 uses.
const capture = (file, tool, args) =>
  fromDir ? readFileSync(join(tokenDump, file))
          : execFileSync(tool, args, { maxBuffer: 1 << 29 });

let failures = 0;
const line = (label, detail, ok) => {
  process.stdout.write(`  ${label.padEnd(34)} ${ok ? 'ok  ' : 'FAIL'}   ${detail}\n`);
  if (!ok) failures++;
};

class Reader {
  constructor(b) { this.b = b; this.o = 0; }
  u8()  { return this.b[this.o++]; }
  u32() { const v = this.b.readUInt32LE(this.o); this.o += 4; return v; }
  i32() { const v = this.b.readInt32LE(this.o);  this.o += 4; return v; }
  f64() { const v = this.b.readDoubleLE(this.o); this.o += 8; return v; }
  u16() { const v = this.b.readUInt16LE(this.o); this.o += 2; return v; }
  str() { const n = this.u32(); const s = this.b.toString('utf8', this.o, this.o + n); this.o += n; return s; }
  get done() { return this.o === this.b.length; }
}

// ---------------------------------------------------------------------------
// 1. Normalization, over every code point there is.
// ---------------------------------------------------------------------------

const HOMOGLYPH_MAP = {
  'Α':'A','Β':'B','Ε':'E','Η':'H','Ι':'I','Κ':'K','Μ':'M','Ν':'N','Ο':'O','Ρ':'P','Τ':'T','Υ':'Y','Ζ':'Z',
  'А':'A','В':'B','С':'C','Е':'E','Н':'H','К':'K','М':'M','О':'O','Р':'P','Т':'T',
  'а':'a','с':'c','е':'e','о':'o','р':'p',
};
const HOMOGLYPH_RE = new RegExp('[' + Object.keys(HOMOGLYPH_MAP).join('') + ']', 'g');
const ZERO_WIDTH_RE = new RegExp(
  '[' + [0x200B, 0x200C, 0x200D, 0x2060, 0xFEFF].map((c) => String.fromCharCode(c)).join('') + ']', 'g');
const normalize = (s) => s.normalize('NFKC').replace(ZERO_WIDTH_RE, '')
  .replace(HOMOGLYPH_RE, (ch) => HOMOGLYPH_MAP[ch] ?? ch);

process.stdout.write('\nNormalization -- exhaustive, every code point\n');
if (fromDir || (normDump && existsSync(normDump))) {
  const buf = capture('norm.bin', normDump, ['--allcp']);
  const r = new Reader(buf);
  const count = r.u32();
  let cp = 0, bad = 0, firstBad = null;
  for (let i = 0; i < count; i++) {
    while (cp >= 0xD800 && cp <= 0xDFFF) cp++;
    const n = r.u16();
    let got = '';
    for (let k = 0; k < n; k++) got += String.fromCharCode(r.u16());
    const want = normalize(String.fromCodePoint(cp));
    if (got !== want) {
      bad++;
      if (!firstBad) firstBad = `U+${cp.toString(16).toUpperCase().padStart(4, '0')}: C ${JSON.stringify(got)} != JS ${JSON.stringify(want)}`;
    }
    cp++;
  }
  line('code points compared', `${count}`, count === 1112064);
  line('stream consumed', `${r.o} bytes`, r.done);
  line('NFKC + strip + homoglyph', bad === 0 ? 'identical' : `${bad} differ, first ${firstBad}`, bad === 0);
} else {
  line('kl_norm_dump', 'not supplied -- normalization not checked', false);
}

// ---------------------------------------------------------------------------
// 2. Number(), over the grid.
// ---------------------------------------------------------------------------

process.stdout.write('\nNumber() -- the one arithmetic step in classifyPart\n');
{
  const grid = numberGrid();
  const blob = join(root, 'goldens', 'numbers.bin');
  const buf = capture('numbers.bin', tokenDump, ['--numbers', blob]);
  const r = new Reader(buf);
  const count = r.u32();
  let exact = 0, differ = 0, firstDiff = null;
  // The window kl_parse_decimal claims to be exact over: mantissa within
  // 2^53 and scale within 10^0..10^22. Outside it the C says so itself, and
  // the grid carries a few such strings deliberately.
  const inWindow = (s) => {
    const m = /^[+-]?(\d*)(?:\.(\d*))?$/.exec(s);
    if (!m) return false;
    const digits = (m[1] ?? '') + (m[2] ?? '');
    const frac = (m[2] ?? '').length;
    return BigInt(digits === '' ? '0' : digits) <= 9007199254740991n && frac <= 22;
  };
  let outOfWindow = 0;
  for (let i = 0; i < count; i++) {
    const got = r.f64();
    const want = Number(grid[i]);
    if (Object.is(got, want)) { exact++; continue; }
    if (!inWindow(grid[i])) { outOfWindow++; continue; }
    differ++;
    if (!firstDiff) firstDiff = `${JSON.stringify(grid[i])}: C ${got} != JS ${want}`;
  }
  line('grid size', `${count}`, count === grid.length);
  line('stream consumed', `${r.o} bytes`, r.done);
  line('exact inside the claimed window', `${exact}/${count - outOfWindow}`, differ === 0);
  if (firstDiff) process.stdout.write(`      first difference: ${firstDiff}\n`);
  process.stdout.write(`      outside the window, not claimed: ${outOfWindow}\n`);
}

// ---------------------------------------------------------------------------
// 3. The tokens.
// ---------------------------------------------------------------------------

// Byte-for-byte the digest goldens.mjs captures, so the comparison is against
// the frozen corpus and not only against a live tokenize().
class Digest {
  constructor() { this.h = createHash('sha256'); }
  u32(x) { const b = Buffer.alloc(4); b.writeUInt32LE(x >>> 0); this.h.update(b); return this; }
  i32(x) { const b = Buffer.alloc(4); b.writeInt32LE(x | 0);   this.h.update(b); return this; }
  f64(x) { const b = Buffer.alloc(8); b.writeDoubleLE(x);      this.h.update(b); return this; }
  u8(x)  { this.h.update(Buffer.from([x & 0xff])); return this; }
  str(s) { const b = Buffer.from(s, 'utf8'); this.u32(b.length); this.h.update(b); return this; }
  hex()  { return this.h.digest('hex'); }
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

process.stdout.write('\nTokens -- every case in the corpus, field by field\n');
{
  const cases = JSON.parse(readFileSync(join(root, 'goldens', 'cases.json'), 'utf8'));
  const blob = join(root, 'goldens', 'cases-text.bin');
  const buf = capture('tokens.bin', tokenDump, [blob]);
  const r = new Reader(buf);
  const count = r.u32();

  let compared = 0, tokensCompared = 0, fieldMismatch = 0, sourceMismatch = 0;
  let digestOk = 0, digestBad = 0;
  const problems = [];

  for (let i = 0; i < count; i++) {
    const c = cases[i];
    const srcLen = r.u32();
    let source = '';
    for (let k = 0; k < srcLen; k++) source += String.fromCharCode(r.u16());

    const n = r.u32();
    const got = [];
    for (let k = 0; k < n; k++) {
      got.push({
        type: r.str(), code: r.str(), key: r.str(), name: r.str(), text: r.str(),
        stressed: r.u8(), transient: r.u8(), relative: r.u8(), reset: r.u8(),
        value: r.f64(), pitchDelta: r.f64(), ms: r.f64(),
        srcStart: r.i32(), srcEnd: r.i32(),
      });
    }

    const ref = tokenize(c.text);
    if (source !== ref.source) {
      sourceMismatch++;
      if (problems.length < 6) problems.push(`${c.id}: source ${JSON.stringify(source)} != ${JSON.stringify(ref.source)}`);
    }
    if (got.length !== ref.tokens.length) {
      fieldMismatch++;
      if (problems.length < 6) problems.push(`${c.id}: ${got.length} tokens, reference has ${ref.tokens.length}`);
    } else {
      for (let k = 0; k < got.length; k++) {
        const a = got[k], b = ref.tokens[k];
        const cmp = [
          ['type', a.type, b.type],
          ['code', a.code, b.code ?? ''],
          ['key', a.key, b.key ?? ''],
          ['name', a.name, b.name ?? ''],
          ['text', a.text, b.text ?? ''],
          ['stressed', a.stressed, b.stressed ? 1 : 0],
          ['transient', a.transient, b.transient ? 1 : 0],
          ['relative', a.relative, b.relative ? 1 : 0],
          ['reset', a.reset, b.reset ? 1 : 0],
          ['value', a.value, b.value ?? 0],
          ['pitchDelta', a.pitchDelta, b.pitchDelta ?? 0],
          ['ms', a.ms, b.ms ?? 0],
          ['srcStart', a.srcStart, b.srcStart ?? -1],
          ['srcEnd', a.srcEnd, b.srcEnd ?? -1],
        ];
        for (const [name, x, y] of cmp) {
          if (!Object.is(x, y)) {
            fieldMismatch++;
            if (problems.length < 6) problems.push(`${c.id} token ${k} ${name}: C ${JSON.stringify(x)} != JS ${JSON.stringify(y)}`);
          }
        }
        tokensCompared++;
      }
    }

    // The frozen goldens. Rebuild the captured digest from the C's own
    // fields; matching it means the C agrees with the corpus as captured,
    // not merely with today's JavaScript.
    const asRef = got.map((t) => ({
      type: t.type,
      code: t.code || undefined, key: t.key || undefined,
      name: t.name || undefined, text: t.text || undefined,
      stressed: !!t.stressed, transient: !!t.transient,
      relative: !!t.relative, reset: !!t.reset,
      value: t.value, pitchDelta: t.pitchDelta, ms: t.ms,
      srcStart: t.srcStart, srcEnd: t.srcEnd,
    }));
    if (digestTokens(asRef) === c.tokens.digest) digestOk++;
    else {
      digestBad++;
      if (problems.length < 6) problems.push(`${c.id}: token digest differs from the golden`);
    }
    compared++;
  }

  line('cases compared', `${compared}`, compared === cases.length);
  line('stream consumed', `${r.o} bytes`, r.done);
  line('tokens compared', `${tokensCompared}`, tokensCompared > 0);
  line('normalized source matches', sourceMismatch === 0 ? 'all' : `${sourceMismatch} differ`, sourceMismatch === 0);
  line('every field identical', fieldMismatch === 0 ? 'all' : `${fieldMismatch} mismatches`, fieldMismatch === 0);
  line('digest matches the goldens', `${digestOk}/${compared}`, digestBad === 0);
  for (const p of problems) process.stdout.write(`      ${p}\n`);
}

process.stdout.write('\n');
// ---------------------------------------------------------------------------
// 4. The deliberate divergences, and the behaviours worth pinning.
// ---------------------------------------------------------------------------

process.stdout.write('\nDivergences -- where the C refuses to copy the reference\n');
{
  const blob = join(root, 'goldens', 'divergences.bin');
  const buf = capture('divergences.bin', tokenDump, [blob]);
  const r = new Reader(buf);
  const count = r.u32();
  const readTokens = () => {
    const srcLen = r.u32();
    for (let k = 0; k < srcLen; k++) r.u16();
    const n = r.u32();
    const toks = [];
    for (let k = 0; k < n; k++) {
      toks.push({
        type: r.str(), code: r.str(), key: r.str(), name: r.str(), text: r.str(),
        stressed: r.u8(), transient: r.u8(), relative: r.u8(), reset: r.u8(),
        value: r.f64(), pitchDelta: r.f64(), ms: r.f64(),
        srcStart: r.i32(), srcEnd: r.i32(),
      });
    }
    return toks;
  };
  const got = [];
  for (let i = 0; i < count; i++) got.push(readTokens());

  line('inputs dumped', `${count}`, count === DIVERGENCES.length + PINNED.length);
  line('stream consumed', `${r.o} bytes`, r.done);

  // Each divergence must still be a divergence, in both directions: the
  // reference must still have the defect, and the C must still decline it.
  let refStillWrong = 0, cStillRight = 0;
  for (let i = 0; i < DIVERGENCES.length; i++) {
    const d = DIVERGENCES[i];
    const ref = tokenize(d.text).tokens;
    const okRef = ref.length === 1 && ref[0].type === d.jsType
      && (d.jsMsIsNotANumber ? typeof ref[0].ms !== 'number' : true);
    if (okRef) refStillWrong++;
    else process.stdout.write(`      upstream no longer misbehaves on ${JSON.stringify(d.text)} -- re-read the divergence\n`);
    const c = got[i];
    if (c.length === 1 && c[0].type === d.cType && c[0].text === d.text) cStillRight++;
    else process.stdout.write(`      C gave ${JSON.stringify(c.map((x) => x.type))} for ${JSON.stringify(d.text)}\n`);
  }
  line('reference still has the defect', `${refStillWrong}/${DIVERGENCES.length}`, refStillWrong === DIVERGENCES.length);
  line('C classifies them as unknown', `${cStillRight}/${DIVERGENCES.length}`, cStillRight === DIVERGENCES.length);

  // PINNED are not divergences. They are behaviours that are easy to get
  // wrong and invisible in the corpus, so they must match exactly.
  let pinnedOk = 0;
  const pinnedProblems = [];
  for (let i = 0; i < PINNED.length; i++) {
    const pin = PINNED[i];
    const ref = tokenize(pin.text).tokens;
    const c = got[DIVERGENCES.length + i];
    let same = c.length === ref.length;
    if (same) {
      for (let k = 0; k < c.length; k++) {
        const a = c[k], b = ref[k];
        if (a.type !== b.type || a.code !== (b.code ?? '') || a.key !== (b.key ?? '')
            || a.name !== (b.name ?? '') || a.text !== (b.text ?? '')
            || a.stressed !== (b.stressed ? 1 : 0) || a.transient !== (b.transient ? 1 : 0)
            || a.relative !== (b.relative ? 1 : 0) || a.reset !== (b.reset ? 1 : 0)
            || !Object.is(a.value, b.value ?? 0) || !Object.is(a.pitchDelta, b.pitchDelta ?? 0)
            || !Object.is(a.ms, b.ms ?? 0)
            || a.srcStart !== (b.srcStart ?? -1) || a.srcEnd !== (b.srcEnd ?? -1)) {
          same = false;
          break;
        }
      }
    }
    if (same) pinnedOk++;
    else pinnedProblems.push(`${JSON.stringify(pin.text)} (${pin.why}): C ${JSON.stringify(c.map((x) => x.type))} vs JS ${JSON.stringify(ref.map((x) => x.type))}`);
  }
  line('pinned behaviours identical', `${pinnedOk}/${PINNED.length}`, pinnedOk === PINNED.length);
  for (const q of pinnedProblems) process.stdout.write(`      ${q}\n`);
}

process.stdout.write('\n');
if (failures) {
  process.stderr.write(`stage 4: ${failures} check(s) failed\n`);
  process.exit(1);
}
process.stdout.write('stage 4: all checks passed\n');
