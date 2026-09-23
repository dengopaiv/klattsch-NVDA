// Convert goldens/schedules.json to a flat binary the C can read without a
// JSON parser.
//
//   node tools/schedules-to-bin.mjs [out.bin]
//
// Stage 3 verifies the sample loop before the C compiler exists, so it needs
// schedules from somewhere. Giving C a JSON parser for the sake of a test
// would put a parser in the product's dependency graph for no product reason,
// so the conversion happens here and the C reads a length-prefixed stream.
//
// Format, little-endian throughout:
//
//   8    magic "KLSCHED\0"
//   u32  version (1)
//   u32  sample-rate count, then that many f64 rates
//   u32  case count
//   per case:
//     u32  id length, then id bytes (UTF-8)
//     f64  totalMs
//     u32  voice count
//     per voice:
//       u32  event count
//       per event:
//         f64  atMs
//         f64  transitionMs
//         u32  present mask over the 19 PARAMS, bit i = PARAMS[i] present
//         f64  one value per set bit, in PARAMS order
//
// Uppercase engine extras ([OQ=...]) are not emitted: FormantSynth ignores
// them, so they cannot affect a sample, and stage 3 is about the sample loop.
// They return when an engine that reads them does.

import { readFileSync, writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { PARAMS } from '../src/engine/index.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');
const outPath = process.argv[2] ?? join(root, 'goldens', 'schedules.bin');

const schedules = JSON.parse(readFileSync(join(root, 'goldens', 'schedules.json'), 'utf8'));
const cases = JSON.parse(readFileSync(join(root, 'goldens', 'cases.json'), 'utf8'));
const manifest = JSON.parse(readFileSync(join(root, 'goldens', 'manifest.json'), 'utf8'));

const totalMsById = new Map();
for (const c of cases) totalMsById.set(c.id, c.compile.totalMs);

if (PARAMS.length > 32) throw new Error('present mask is 32 bits; PARAMS has outgrown it');

const chunks = [];
const push = (b) => chunks.push(b);
const u32 = (v) => { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0, 0); push(b); };
const f64 = (v) => { const b = Buffer.alloc(8); b.writeDoubleLE(v, 0); push(b); };
const str = (s) => { const b = Buffer.from(s, 'utf8'); u32(b.length); push(b); };

push(Buffer.from('KLSCHED\0', 'latin1'));
u32(1);

const rates = manifest.audioRates;
u32(rates.length);
for (const r of rates) f64(r);

const ids = Object.keys(schedules).sort();
u32(ids.length);

let events = 0;
let values = 0;

for (const id of ids) {
  str(id);
  const totalMs = totalMsById.get(id);
  if (totalMs == null) throw new Error(`no totalMs for ${id}`);
  f64(totalMs);

  const voices = schedules[id];
  u32(voices.length);
  for (const voice of voices) {
    u32(voice.length);
    for (const evt of voice) {
      f64(evt.atMs ?? 0);
      f64(evt.transitionMs ?? 30);
      let mask = 0;
      for (let i = 0; i < PARAMS.length; i++) {
        if (PARAMS[i] in evt.target) mask |= (1 << i);
      }
      u32(mask);
      for (let i = 0; i < PARAMS.length; i++) {
        if (mask & (1 << i)) { f64(evt.target[PARAMS[i]]); values++; }
      }
      events++;
    }
  }
}

const buf = Buffer.concat(chunks);
writeFileSync(outPath, buf);
process.stdout.write(
  `wrote ${outPath}: ${ids.length} cases, ${events} events, ${values} values, `
  + `${(buf.length / 1024).toFixed(1)} KB\n`);
