// Flatten the golden corpus's input texts to a length-prefixed binary blob,
// so the C needs no JSON parser to be driven by it. Same reasoning as
// tools/schedules-to-bin.mjs at stage 3: a parser in the test is a parser in
// the product's dependency graph for no product reason.
//
//   node tools/cases-to-bin.mjs              -> goldens/cases-text.bin
//   node tools/cases-to-bin.mjs --numbers    -> goldens/numbers.bin (the Number() grid)
//   node tools/cases-to-bin.mjs --divergence -> goldens/divergences.bin
//   node tools/cases-to-bin.mjs --compile    -> goldens/cases-compile.bin

import { readFileSync, writeFileSync } from 'node:fs';
import { ALL as DIVERGENCE_INPUTS } from './stage4-divergences.mjs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');

function pack(strings) {
  const parts = [Buffer.alloc(4)];
  parts[0].writeUInt32LE(strings.length);
  for (const s of strings) {
    const b = Buffer.from(s, 'utf8');
    const h = Buffer.alloc(4);
    h.writeUInt32LE(b.length);
    parts.push(h, b);
  }
  return Buffer.concat(parts);
}

// The grid the Number() comparison walks. Shapes the grammar can produce,
// plus the edges of the exact window kl_parse_decimal claims.
export function numberGrid() {
  const out = [];
  for (let i = 0; i <= 999; i++) out.push(String(i));
  for (const s of ['', '-', '+']) {
    for (const i of [0, 1, 7, 9, 10, 99, 100, 120, 440, 1000, 32767, 99999, 123456789]) {
      out.push(s + i);
      for (const f of ['0', '1', '5', '25', '125', '0001', '999999', '3333333333333333']) {
        out.push(`${s}${i}.${f}`);
      }
    }
  }
  // The boundaries of the claim: 2^53, one digit past it, and 10^22.
  for (const s of ['9007199254740992', '9007199254740993', '9007199254740991',
                   '1.0000000000000002', '0.1', '0.2', '0.3', '1e0'.replace('e0',''),
                   '123456789012345678901234567890',
                   '0.0000000000000000000001', '0.00000000000000000000001',
                   '1.7976931348623157', '2.2250738585072014',
                   '00000000000000000000001', '0.000', '-0', '-0.0']) out.push(s);
  return out;
}

// The ten scalar options compileSection reads, in the order kl_opt declares
// them in csrc/kl_compile.h. The order is part of the wire format between
// this and kl_compile_dump, so it is written down once and shared rather
// than repeated in two places that can drift.
export const OPT_NAMES = [
  'baseF0', 'rate', 'scale',
  'vibratoDepth', 'vibratoRate',
  'tremoloDepth', 'tremoloRate',
  'aspiration', 'tilt', 'effort',
];

// Stage 5 needs each case's opts as well as its text: `bank`, `extras` and
// `engine` all change what the compiler produces, and 262 of the 714 cases
// carry one. `gain` is in the corpus too and is deliberately not here -- it
// is a renderer option that compileSection never reads.
export function packCompileCases(cases) {
  const parts = [Buffer.alloc(4)];
  parts[0].writeUInt32LE(cases.length);
  const u32 = (n) => { const b = Buffer.alloc(4); b.writeUInt32LE(n); return b; };
  const f64 = (x) => { const b = Buffer.alloc(8); b.writeDoubleLE(x); return b; };
  const str = (s) => { const b = Buffer.from(s ?? '', 'utf8'); return [u32(b.length), b]; };

  for (const c of cases) {
    const o = c.opts ?? {};
    parts.push(...str(c.text));

    let mask = 0;
    const vals = [];
    OPT_NAMES.forEach((name, i) => {
      if (o[name] !== undefined) mask |= (1 << i);
      vals.push(o[name] ?? 0);
    });
    parts.push(u32(mask));
    for (const v of vals) parts.push(f64(v));

    parts.push(...str(o.bank));
    parts.push(...str(o.engine));

    const extras = Object.entries(o.extras ?? {});
    parts.push(u32(extras.length));
    for (const [k, v] of extras) { parts.push(...str(k)); parts.push(f64(v)); }
  }
  return Buffer.concat(parts);
}

// Only when run as a program. verify-stage4.mjs imports numberGrid() from
// here, and an import that rewrites the corpus as a side effect is a test
// that quietly changes its own inputs.
const runAsScript = process.argv[1]
  && fileURLToPath(import.meta.url) === process.argv[1];

const wantNumbers = process.argv.includes('--numbers');
if (!runAsScript) {
  // nothing to do -- numberGrid() is the export
} else if (process.argv.includes('--divergence')) {
  writeFileSync(join(root, 'goldens', 'divergences.bin'), pack(DIVERGENCE_INPUTS));
  process.stdout.write(`wrote goldens/divergences.bin  ${DIVERGENCE_INPUTS.length} inputs
`);
} else if (process.argv.includes('--compile')) {
  const cases = JSON.parse(readFileSync(join(root, 'goldens', 'cases.json'), 'utf8'));
  writeFileSync(join(root, 'goldens', 'cases-compile.bin'), packCompileCases(cases));
  process.stdout.write(`wrote goldens/cases-compile.bin  ${cases.length} cases
`);
} else if (wantNumbers) {
  const grid = numberGrid();
  writeFileSync(join(root, 'goldens', 'numbers.bin'), pack(grid));
  process.stdout.write(`wrote goldens/numbers.bin  ${grid.length} strings\n`);
} else {
  const cases = JSON.parse(readFileSync(join(root, 'goldens', 'cases.json'), 'utf8'));
  const texts = cases.map((c) => c.text);
  writeFileSync(join(root, 'goldens', 'cases-text.bin'), pack(texts));
  process.stdout.write(`wrote goldens/cases-text.bin  ${texts.length} texts\n`);
}
