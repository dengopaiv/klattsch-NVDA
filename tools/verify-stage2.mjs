// Stage 2 exit test: the compiled-in banks against the JavaScript's resolved
// banks, field by field. See docs/14-stage2-banks.md.
//
//   node tools/verify-stage2.mjs <path-to-kl_banks_dump>
//   node tools/verify-stage2.mjs <dir-of-dumped-sections>
//
// Everything here is Tier 1: bank data is numbers copied from a table, with no
// arithmetic of any kind between the JSON and the C. A single differing field
// is a bug, and there is no tolerance to hide behind.
//
// The comparison is field by field rather than by digest so a failure says
// *which* phoneme and *which* field, which is the difference between a
// five-minute fix and an afternoon.

import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { banks, registerBank } from '../src/engine/index.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');

const target = process.argv[2];
if (!target || !existsSync(target)) {
  process.stderr.write(
    'usage: node tools/verify-stage2.mjs <path-to-kl_banks_dump>\n'
    + '       node tools/verify-stage2.mjs <dir-of-dumped-sections>\n');
  process.exit(2);
}
const fromDir = statSync(target).isDirectory();

function dump(section) {
  if (fromDir) return readFileSync(join(target, `${section}.bin`));
  return execFileSync(target, [section], { maxBuffer: 1 << 26 });
}

let failures = 0;
const problems = [];
const line = (label, detail, ok) => {
  process.stdout.write(`  ${label.padEnd(38)} ${ok ? 'ok' : 'FAIL'}   ${detail}\n`);
  if (!ok) failures++;
};
const note = (msg) => { problems.push(msg); if (problems.length <= 12) process.stdout.write(`      ${msg}\n`); };

// --- reader ----------------------------------------------------------------

class Reader {
  constructor(buf) { this.b = buf; this.o = 0; }
  u32() { const v = this.b.readUInt32LE(this.o); this.o += 4; return v; }
  i32() { const v = this.b.readInt32LE(this.o); this.o += 4; return v; }
  u8() { return this.b[this.o++]; }
  f64() { const v = this.b.readDoubleLE(this.o); this.o += 8; return v; }
  str() {
    const n = this.u32();
    if (n === 0xFFFFFFFF) return null;      // NULL, distinct from ""
    const s = this.b.toString('utf8', this.o, this.o + n);
    this.o += n;
    return s;
  }
  get done() { return this.o === this.b.length; }
}

const NUMERIC = ['voicing', 'F1', 'F2', 'F3', 'BW1', 'BW2', 'BW3', 'A1', 'A2', 'A3'];

// --- the banks themselves ---------------------------------------------------

process.stdout.write('Bank data -- Tier 1, exact\n');

{
  const r = new Reader(dump('banks'));
  const jsNames = banks.list().sort();

  const count = r.u32();
  line('bank count', `${count}`, count === jsNames.length);

  const def = r.str();
  line('default bank', `${def}`, def === banks.defaultName);

  let fieldsCompared = 0;
  let phonemesCompared = 0;

  for (let i = 0; i < count; i++) {
    const name = r.str();
    const displayName = r.str();
    const language = r.str();
    const license = r.str();
    const source = r.str();
    const schemaVersion = r.i32();
    const phCount = r.u32();

    const js = banks.get(name);
    if (!js) { line(`bank[${i}] ${name}`, 'no such bank in the JS', false); break; }

    const meta = [
      ['name', name, js.name],
      ['displayName', displayName, js.displayName],
      ['language', language, js.language ?? null],
      ['license', license, js.license ?? null],
      ['source', source, js.source ?? null],
      ['schemaVersion', schemaVersion, js.schemaVersion],
    ];
    let metaOk = true;
    for (const [k, got, want] of meta) {
      fieldsCompared++;
      if (got !== want) { metaOk = false; note(`${name}.${k}: C ${JSON.stringify(got)} != JS ${JSON.stringify(want)}`); }
    }
    line(`bank[${i}] ${name} metadata`, `${meta.length} fields`, metaOk);

    const jsCodes = Object.keys(js.phonemes).sort();
    let phOk = phCount === jsCodes.length;
    if (!phOk) note(`${name}: C has ${phCount} phonemes, JS has ${jsCodes.length}`);

    for (let j = 0; j < phCount; j++) {
      const code = r.str();
      const nums = NUMERIC.map(() => r.f64());
      const isStop = r.u8();
      const hasGlide = r.u8();
      const g = [r.f64(), r.f64(), r.f64()];
      const ipa = r.str();
      const example = r.str();
      const src = r.str();

      const want = js.phonemes[code];
      phonemesCompared++;
      if (!want) { phOk = false; note(`${name}: C has phoneme ${JSON.stringify(code)}, JS does not`); continue; }

      // Sort order, which the binary search depends on.
      if (code !== jsCodes[j]) { phOk = false; note(`${name}[${j}]: C code ${code} != sorted JS ${jsCodes[j]}`); }

      NUMERIC.forEach((k, n) => {
        fieldsCompared++;
        // Exact: these are copied, never computed. `?? 0` matches the
        // generator, which writes 0 for an absent numeric field.
        const w = want[k] ?? 0;
        if (nums[n] !== w) { phOk = false; note(`${name}/${code}.${k}: C ${nums[n]} != JS ${w}`); }
      });

      fieldsCompared++;
      if (!!isStop !== !!want.isStop) { phOk = false; note(`${name}/${code}.isStop: C ${isStop} != JS ${!!want.isStop}`); }

      fieldsCompared++;
      const wantGlide = !!want.glideTo;
      if (!!hasGlide !== wantGlide) { phOk = false; note(`${name}/${code}.hasGlide: C ${hasGlide} != JS ${wantGlide}`); }
      if (wantGlide) {
        ['F1', 'F2', 'F3'].forEach((k, n) => {
          fieldsCompared++;
          if (g[n] !== want.glideTo[k]) { phOk = false; note(`${name}/${code}.glideTo.${k}: C ${g[n]} != JS ${want.glideTo[k]}`); }
        });
      }

      // Provenance. A dropped `source` is a licensing problem, not a cosmetic
      // one, so it is compared as strictly as a formant.
      for (const [k, got, w] of [['ipa', ipa, want.ipa ?? null], ['example', example, want.example ?? null], ['source', src, want.source ?? null]]) {
        fieldsCompared++;
        if (got !== w) { phOk = false; note(`${name}/${code}.${k}: C ${JSON.stringify(got)} != JS ${JSON.stringify(w)}`); }
      }
    }
    line(`bank[${i}] ${name} phonemes`, `${phCount} entries`, phOk);
  }

  line('stream fully consumed', `${r.o} bytes`, r.done);
  line('fields compared', `${fieldsCompared} across ${phonemesCompared} phonemes`, fieldsCompared > 0);
}

// --- the lookup functions ---------------------------------------------------

process.stdout.write('\nLookup\n');

{
  const r = new Reader(dump('probe'));
  const count = r.u32();
  let allHits = true, allMisses = true, identity = true;
  for (let i = 0; i < count; i++) {
    if (!r.u8()) identity = false;
    const n = r.u32();
    for (let j = 0; j < n; j++) if (!r.u8()) allHits = false;
    const m = r.u32();
    for (let j = 0; j < m; j++) if (!r.u8()) allMisses = false;
  }
  line('kl_bank_get returns the table row', 'identity, not a copy', identity);
  line('every code is findable', 'binary search over all banks', allHits);
  line('absent codes miss', 'empty, lowercase, spaced, out-of-range', allMisses);
  line('unknown bank is NULL', '', !!r.u8());
  line('NULL bank name is NULL', 'no crash', !!r.u8());
  line('NULL bank to find is NULL', 'no crash', !!r.u8());
  line('NULL code to find is NULL', 'no crash', !!r.u8());
  line('default bank exists', '', !!r.u8());
  const defName = r.str();
  line('default bank name', defName, defName === banks.defaultName);
  line('probe stream consumed', `${r.o} bytes`, r.done);
}

// --- extends and null deletion ---------------------------------------------
//
// No shipped bank uses `null` deletion, so the resolver path that handles it
// is not exercised by the data at all. The C inherits its correctness from
// this resolver -- the generator emits what it produces -- so the resolver is
// tested here directly with a fixture, and the chapter says so.

process.stdout.write('\nextends and null deletion (fixture; no shipped bank uses null)\n');

{
  registerBank({
    schemaVersion: 1,
    name: '_stage2-fixture',
    displayName: 'stage 2 fixture',
    extends: 'klatt1980-en',
    phonemes: {
      AA: null,                                   // delete an inherited entry
      IY: { voicing: 1, F1: 111, F2: 222, F3: 333, BW1: 1, BW2: 2, BW3: 3, A1: 1, A2: 1, A3: 1 },
      QQ: { voicing: 0, F1: 9, F2: 8, F3: 7, BW1: 6, BW2: 5, BW3: 4, A1: 0, A2: 0, A3: 0 },
    },
  });
  const base = banks.get('klatt1980-en');
  const fx = banks.get('_stage2-fixture');

  line('inherits from the parent', `${Object.keys(fx.phonemes).length} entries`,
    Object.keys(fx.phonemes).length === Object.keys(base.phonemes).length - 1 + 1);
  line('null deletes an inherited entry', 'AA removed', !('AA' in fx.phonemes));
  line('child overrides the parent', `IY.F1 = ${fx.phonemes.IY?.F1}`, fx.phonemes.IY?.F1 === 111);
  line('child adds a new entry', 'QQ present', fx.phonemes.QQ?.F3 === 7);
  line('parent is untouched', `AA.F1 = ${base.phonemes.AA?.F1}`, base.phonemes.AA?.F1 === 700);
}

process.stdout.write('\n');
if (problems.length > 12) {
  process.stdout.write(`  ... and ${problems.length - 12} more differences\n\n`);
}
if (failures) {
  process.stderr.write(`stage 2: ${failures} check(s) failed\n`);
  process.exit(1);
}
process.stdout.write('stage 2: all checks passed\n');
