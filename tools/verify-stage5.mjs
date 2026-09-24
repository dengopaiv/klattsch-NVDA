// Stage 5 exit test: csrc/kl_compile.c against the JavaScript compiler it
// came from. See docs/17-stage5-compile.md.
//
//   node tools/verify-stage5.mjs <kl_compile_dump>
//   node tools/verify-stage5.mjs <directory-of-dumps>
//
// Tier 1, no tolerance. Every quantity here is an exact IEEE-754 double, an
// integer or a string, and every comparison is Object.is or ===. The only
// transcendental the compiler can reach is noteToHz, and that is spent in the
// tokenizer before this stage sees it -- by the time a value arrives here it
// is a number the token already carried, so stage 5 compares it exactly
// rather than to 1e-9 as REWRITE.md's Tier 1 allowed for.
//
// Four things are checked, in the order a failure is easiest to read in:
//
//   1. the top level -- totalMs, voice count, engine, and the merged warnings,
//   2. every voice's schedule, event by event and field by field,
//   3. every voice's phrases,
//   4. the digests, against the frozen goldens rather than against a live
//      compile. (2) says *what* differs; (4) ties the C to the reference as
//      it was captured rather than to whatever the JavaScript does today.
//      Stage 1 learned that the first without the second is a comparison with
//      no reference in it.

import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { compile, tokenize } from '../src/engine/sequencer.js';
import { PARAMS } from '../src/engine/synth-core.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');

const dumpTool = process.argv[2];
if (!dumpTool || !existsSync(dumpTool)) {
  process.stderr.write('usage: node tools/verify-stage5.mjs <kl_compile_dump>\n');
  process.exit(2);
}
const fromDir = statSync(dumpTool).isDirectory();

// Either run the tool, or read what it wrote somewhere else. WSL's Debian has
// the only independent libm on this machine and no Node, so its dumps are
// captured there and compared here -- the arrangement stages 3 and 4 use.
const capture = (file, tool, args) =>
  fromDir ? readFileSync(join(dumpTool, file))
          : execFileSync(tool, args, { maxBuffer: 1 << 29 });

let failures = 0;
const line = (label, detail, ok) => {
  process.stdout.write(`  ${label.padEnd(38)} ${ok ? 'ok  ' : 'FAIL'}   ${detail}\n`);
  if (!ok) failures++;
};

class Reader {
  constructor(b) { this.b = b; this.o = 0; }
  u8()  { return this.b[this.o++]; }
  u32() { const v = this.b.readUInt32LE(this.o); this.o += 4; return v; }
  i32() { const v = this.b.readInt32LE(this.o);  this.o += 4; return v; }
  f64() { const v = this.b.readDoubleLE(this.o); this.o += 8; return v; }
  str() { const n = this.u32(); const s = this.b.toString('utf8', this.o, this.o + n); this.o += n; return s; }
  get done() { return this.o === this.b.length; }
}

// The same canonical digest goldens.mjs uses. Duplicated rather than imported
// because goldens.mjs is a program with side effects, and because a digest
// the exit test computes for itself cannot be quietly changed by editing the
// generator.
class Digest {
  constructor() { this.h = createHash('sha256'); this.b = Buffer.alloc(8); }
  f64(x) { this.b.writeDoubleLE(x, 0); this.h.update(this.b); return this; }
  u32(x) { const b = Buffer.alloc(4); b.writeUInt32LE(x >>> 0, 0); this.h.update(b); return this; }
  i32(x) { const b = Buffer.alloc(4); b.writeInt32LE(x | 0, 0); this.h.update(b); return this; }
  u8(x)  { this.h.update(Buffer.from([x & 0xff])); return this; }
  str(s) { const b = Buffer.from(s, 'utf8'); this.u32(b.length); this.h.update(b); return this; }
  hex() { return this.h.digest('hex'); }
}

// A C-side event digested exactly as goldens.mjs digests a JS target.
function digestCSchedule(events) {
  const d = new Digest();
  d.u32(events.length);
  for (const e of events) {
    d.f64(e.atMs);
    d.f64(e.transitionMs);
    for (let i = 0; i < PARAMS.length; i++) {
      if (e.present[i]) { d.u8(1); d.f64(e.value[i]); }
      else d.u8(0);
    }
    d.u32(e.extras.length);
    for (const [k, v] of e.extras) { d.str(k); d.f64(v); }
  }
  return d.hex();
}

function digestCPhrases(phrases) {
  const d = new Digest();
  d.u32(phrases.length);
  for (const p of phrases) {
    d.i32(p.srcStart); d.i32(p.srcEnd); d.i32(p.tokenSrcStart);
    d.f64(p.tStartMs); d.f64(p.tEndMs);
    d.str(p.kind); d.str(p.phoneme);
  }
  return d.hex();
}

// ---------------------------------------------------------------------------
// Read the C dump
// ---------------------------------------------------------------------------

const cases = JSON.parse(readFileSync(join(root, 'goldens', 'cases.json'), 'utf8'));
const blob = join(root, 'goldens', 'cases-compile.bin');
if (!existsSync(blob)) {
  process.stderr.write('goldens/cases-compile.bin is missing; run node tools/cases-to-bin.mjs --compile\n');
  process.exit(2);
}

process.stdout.write('stage 5: the schedule compiler\n\n');

const r = new Reader(capture('compile.bin', dumpTool, [blob]));
const nCases = r.u32();
if (nCases !== cases.length) {
  process.stderr.write(`case count mismatch: C ${nCases}, corpus ${cases.length}\n`);
  process.exit(1);
}

const cResults = [];
for (let i = 0; i < nCases; i++) {
  const totalMs = r.f64();
  const nVoices = r.u32();

  const nWarn = r.u32();
  const warnings = [];
  for (let k = 0; k < nWarn; k++) warnings.push(r.str());

  const voices = [];
  for (let v = 0; v < nVoices; v++) {
    const vTotalMs = r.f64();
    const hasEngine = r.u8();
    const engineText = r.str();
    const engine = hasEngine ? engineText : null;

    const nEvents = r.u32();
    const events = [];
    for (let k = 0; k < nEvents; k++) {
      const atMs = r.f64();
      const transitionMs = r.f64();
      const present = [];
      const value = [];
      for (let j = 0; j < PARAMS.length; j++) {
        const p = r.u8();
        present.push(p === 1);
        value.push(p === 1 ? r.f64() : undefined);
      }
      const nExtras = r.u32();
      const extras = [];
      for (let j = 0; j < nExtras; j++) extras.push([r.str(), r.f64()]);
      events.push({ atMs, transitionMs, present, value, extras });
    }

    const nPhrases = r.u32();
    const phrases = [];
    for (let k = 0; k < nPhrases; k++) {
      phrases.push({
        srcStart: r.i32(), srcEnd: r.i32(), tokenSrcStart: r.i32(),
        tStartMs: r.f64(), tEndMs: r.f64(),
        kind: r.str(), phoneme: r.str(),
      });
    }
    voices.push({ totalMs: vTotalMs, engine, events, phrases });
  }
  cResults.push({ totalMs, voices, warnings });
}

if (!r.done) {
  process.stderr.write(`trailing bytes in the C dump: ${r.b.length - r.o}\n`);
  process.exit(1);
}

// ---------------------------------------------------------------------------
// 1. The top level
// ---------------------------------------------------------------------------

process.stdout.write('  the top level\n');
{
  let totalOk = 0, voiceOk = 0, warnOk = 0, engineOk = 0;
  const problems = [];
  for (let i = 0; i < cases.length; i++) {
    const c = cases[i];
    const js = compile(tokenize(c.text), c.opts ?? {});
    const cc = cResults[i];

    if (Object.is(js.totalMs, cc.totalMs)) totalOk++;
    else problems.push(`${c.id}: totalMs JS ${js.totalMs} vs C ${cc.totalMs}`);

    if (js.voices.length === cc.voices.length) voiceOk++;
    else problems.push(`${c.id}: voices JS ${js.voices.length} vs C ${cc.voices.length}`);

    const jw = js.warnings, cw = cc.warnings;
    if (jw.length === cw.length && jw.every((w, k) => w === cw[k])) warnOk++;
    else problems.push(`${c.id}: warnings JS ${JSON.stringify(jw)} vs C ${JSON.stringify(cw)}`);

    const je = js.engine ?? null;
    const ce = cc.voices.length ? cc.voices[0].engine : null;
    if (je === ce) engineOk++;
    else problems.push(`${c.id}: engine JS ${JSON.stringify(je)} vs C ${JSON.stringify(ce)}`);
  }
  line('totalMs exact', `${totalOk}/${cases.length}`, totalOk === cases.length);
  line('voice count', `${voiceOk}/${cases.length}`, voiceOk === cases.length);
  line('warning strings identical', `${warnOk}/${cases.length}`, warnOk === cases.length);
  line('engine marker', `${engineOk}/${cases.length}`, engineOk === cases.length);
  for (const p of problems.slice(0, 12)) process.stdout.write(`      ${p}\n`);
  if (problems.length > 12) process.stdout.write(`      ... and ${problems.length - 12} more\n`);
}

// ---------------------------------------------------------------------------
// 2. Every voice's schedule, field by field
// ---------------------------------------------------------------------------

process.stdout.write('\n  the schedule, field by field\n');
{
  let events = 0, fields = 0;
  let eventCountOk = 0, nVoiceChecked = 0;
  const problems = [];

  for (let i = 0; i < cases.length; i++) {
    const c = cases[i];
    const js = compile(tokenize(c.text), c.opts ?? {});
    const cc = cResults[i];
    const nv = Math.min(js.voices.length, cc.voices.length);

    for (let v = 0; v < nv; v++) {
      nVoiceChecked++;
      const jsSch = js.voices[v].schedule;
      const cSch = cc.voices[v].events;
      if (jsSch.length !== cSch.length) {
        problems.push(`${c.id} voice ${v}: events JS ${jsSch.length} vs C ${cSch.length}`);
        continue;
      }
      eventCountOk++;

      if (!Object.is(js.voices[v].totalMs, cc.voices[v].totalMs)) {
        problems.push(`${c.id} voice ${v}: totalMs JS ${js.voices[v].totalMs} vs C ${cc.voices[v].totalMs}`);
      }
      const jve = js.voices[v].engine ?? null;
      if (jve !== cc.voices[v].engine) {
        problems.push(`${c.id} voice ${v}: engine JS ${JSON.stringify(jve)} vs C ${JSON.stringify(cc.voices[v].engine)}`);
      }

      for (let k = 0; k < jsSch.length; k++) {
        const je = jsSch[k], ce = cSch[k];
        events++;

        if (!Object.is(je.atMs, ce.atMs)) {
          problems.push(`${c.id} voice ${v} event ${k}: atMs JS ${je.atMs} vs C ${ce.atMs}`);
        } else fields++;
        if (!Object.is(je.transitionMs, ce.transitionMs)) {
          problems.push(`${c.id} voice ${v} event ${k}: transitionMs JS ${je.transitionMs} vs C ${ce.transitionMs}`);
        } else fields++;

        for (let j = 0; j < PARAMS.length; j++) {
          const name = PARAMS[j];
          const jHas = name in je.target;
          if (jHas !== ce.present[j]) {
            problems.push(`${c.id} voice ${v} event ${k}: ${name} present JS ${jHas} vs C ${ce.present[j]}`);
            continue;
          }
          if (!jHas) { fields++; continue; }
          if (!Object.is(je.target[name], ce.value[j])) {
            problems.push(`${c.id} voice ${v} event ${k}: ${name} JS ${je.target[name]} vs C ${ce.value[j]}`);
          } else fields++;
        }

        // The engine-specific extras, sorted, exactly as the digest takes
        // them. `isStop`, `glideTo`, `ipa`, `example` and `source` are the
        // documented non-parameters that ride in from the phoneme spread and
        // are excluded here for the reason written down in goldens.mjs.
        const NON_PARAM = new Set(['isStop', 'glideTo', 'ipa', 'example', 'source']);
        const jExtras = Object.keys(je.target)
          .filter((x) => !PARAMS.includes(x) && !NON_PARAM.has(x))
          .sort()
          .map((x) => [x, je.target[x]]);
        const cExtras = ce.extras;
        if (jExtras.length !== cExtras.length
            || jExtras.some((e, n) => e[0] !== cExtras[n][0] || !Object.is(e[1], cExtras[n][1]))) {
          problems.push(`${c.id} voice ${v} event ${k}: extras JS ${JSON.stringify(jExtras)} vs C ${JSON.stringify(cExtras)}`);
        } else fields++;
      }
    }
  }
  line('event counts', `${eventCountOk}/${nVoiceChecked} voices`, eventCountOk === nVoiceChecked);
  line('every target field exact', `${fields} fields over ${events} events`, problems.length === 0);
  for (const p of problems.slice(0, 12)) process.stdout.write(`      ${p}\n`);
  if (problems.length > 12) process.stdout.write(`      ... and ${problems.length - 12} more\n`);
}

// ---------------------------------------------------------------------------
// 3. Phrases
// ---------------------------------------------------------------------------

process.stdout.write('\n  the phrase spans\n');
{
  let phrases = 0;
  const problems = [];
  for (let i = 0; i < cases.length; i++) {
    const c = cases[i];
    const js = compile(tokenize(c.text), c.opts ?? {});
    const cc = cResults[i];
    const nv = Math.min(js.voices.length, cc.voices.length);
    for (let v = 0; v < nv; v++) {
      const jp = js.voices[v].phrases, cp = cc.voices[v].phrases;
      if (jp.length !== cp.length) {
        problems.push(`${c.id} voice ${v}: phrases JS ${jp.length} vs C ${cp.length}`);
        continue;
      }
      for (let k = 0; k < jp.length; k++) {
        const a = jp[k], b = cp[k];
        phrases++;
        if (a.srcStart !== b.srcStart || a.srcEnd !== b.srcEnd
            || a.tokenSrcStart !== b.tokenSrcStart
            || !Object.is(a.tStartMs, b.tStartMs) || !Object.is(a.tEndMs, b.tEndMs)
            || a.kind !== b.kind || (a.phoneme ?? '') !== b.phoneme) {
          problems.push(`${c.id} voice ${v} phrase ${k}: JS ${JSON.stringify(a)} vs C ${JSON.stringify(b)}`);
        }
      }
    }
  }
  line('phrase spans identical', `${phrases} phrases`, problems.length === 0);
  for (const p of problems.slice(0, 12)) process.stdout.write(`      ${p}\n`);
  if (problems.length > 12) process.stdout.write(`      ... and ${problems.length - 12} more\n`);
}

// ---------------------------------------------------------------------------
// 4. Digests, against the frozen goldens
// ---------------------------------------------------------------------------

process.stdout.write('\n  against the frozen goldens\n');
{
  let schedOk = 0, phraseOk = 0, perVoiceOk = 0, perVoiceTotal = 0;
  const problems = [];
  for (let i = 0; i < cases.length; i++) {
    const c = cases[i];
    const cc = cResults[i];
    const g = c.compile;

    const sd = digestCSchedule(cc.voices[0].events);
    if (sd === g.scheduleDigest) schedOk++;
    else problems.push(`${c.id}: scheduleDigest golden ${g.scheduleDigest.slice(0, 16)} vs C ${sd.slice(0, 16)}`);

    const pd = digestCPhrases(cc.voices[0].phrases);
    if (pd === g.phrasesDigest) phraseOk++;
    else problems.push(`${c.id}: phrasesDigest golden ${g.phrasesDigest.slice(0, 16)} vs C ${pd.slice(0, 16)}`);

    for (let v = 0; v < g.voices.length && v < cc.voices.length; v++) {
      perVoiceTotal++;
      if (digestCSchedule(cc.voices[v].events) === g.voices[v].scheduleDigest) perVoiceOk++;
      else problems.push(`${c.id} voice ${v}: per-voice scheduleDigest differs`);
    }
  }
  line('schedule digest (voice 0)', `${schedOk}/${cases.length}`, schedOk === cases.length);
  line('phrases digest (voice 0)', `${phraseOk}/${cases.length}`, phraseOk === cases.length);
  line('schedule digest, every voice', `${perVoiceOk}/${perVoiceTotal}`, perVoiceOk === perVoiceTotal);
  for (const p of problems.slice(0, 12)) process.stdout.write(`      ${p}\n`);
  if (problems.length > 12) process.stdout.write(`      ... and ${problems.length - 12} more\n`);
}

process.stdout.write('\n');
if (failures) {
  process.stderr.write(`stage 5: ${failures} check(s) failed\n`);
  process.exit(1);
}
process.stdout.write('stage 5: all checks passed\n');
