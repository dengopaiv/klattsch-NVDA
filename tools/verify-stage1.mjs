// Stage 1 exit test: csrc/kl_dsp.c against the JavaScript it was translated
// from. See docs/13-stage1-dsp.md.
//
//   node tools/verify-stage1.mjs <path-to-kl_dsp_dump>
//
// Two classes of comparison, because the acceptance criterion in
// docs/REWRITE.md is split and this is the stage where that first bites:
//
//   Tier 1, exact      integer and rational arithmetic -- the LFSR, softClip,
//                      and the coefficient-cache behaviour. Compared by
//                      SHA-256 over the canonical byte stream. Any difference
//                      is a bug.
//
//   Tier 2, bounded    anything that calls sin or cos -- the glottal pulse and
//                      the biquad coefficients. V8 uses its own fdlibm-derived
//                      implementations rather than the platform libm, so two
//                      correct implementations differ in the last bits. These
//                      are compared value by value against a live recomputation
//                      from the JS engine, and must agree to 1e-12.
//
// A digest cannot express a tolerance, so the digests in goldens/primitives.json
// are NOT the acceptance bar for the Tier 2 sections. They are tamper-evidence
// that the JavaScript side has not moved: `goldens.mjs --check` is what proves
// that, and it runs first here.

import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { glottalPulse, BandpassBiquad } from '../src/engine/index.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');

const exe = process.argv[2];
if (!exe || !existsSync(exe)) {
  process.stderr.write('usage: node tools/verify-stage1.mjs <path-to-kl_dsp_dump>\n');
  process.exit(2);
}

const TOLERANCE = 1e-12;
let failures = 0;
const line = (label, detail, ok) => {
  process.stdout.write(`  ${label.padEnd(34)} ${ok ? 'ok' : 'FAIL'}   ${detail}\n`);
  if (!ok) failures++;
};

function dump(section) {
  return execFileSync(exe, [section], { maxBuffer: 1 << 28 });
}

// --- the JavaScript side has not moved --------------------------------------

process.stdout.write('goldens unchanged\n');
try {
  execFileSync(process.execPath, [join(root, 'tools', 'goldens.mjs'), '--check'], { stdio: 'pipe' });
  line('goldens.mjs --check', 'JS engine matches the captured baseline', true);
} catch {
  line('goldens.mjs --check', 'JS ENGINE HAS CHANGED -- fix that before reading anything below', false);
}

const primitives = JSON.parse(readFileSync(join(root, 'goldens', 'primitives.json'), 'utf8'));

// --- Tier 1: exact ----------------------------------------------------------

process.stdout.write('\nTier 1 -- exact, no tolerance\n');

{
  const bytes = dump('lfsr');
  const expectedBytes = 1_000_000 * 4;
  line('xorshift length', `${bytes.length} bytes`, bytes.length === expectedBytes);
  const digest = createHash('sha256').update(bytes).digest('hex');
  line('xorshift 1,000,000 states', digest.slice(0, 16) + '...',
    digest === primitives.xorshift.digest);

  // Name the first divergence rather than only reporting a digest mismatch:
  // a wrong shift and a signed/unsigned slip fail at very different indices.
  if (digest !== primitives.xorshift.digest) {
    let state = 0xACE1ACE1 | 0;
    for (let i = 0; i < 1_000_000; i++) {
      state = ((s) => { let x = s | 0; x ^= x << 13; x ^= x >>> 17; x ^= x << 5; return x | 0; })(state);
      if (bytes.readInt32LE(i * 4) !== state) {
        process.stdout.write(`      first difference at state ${i}: C ${bytes.readInt32LE(i * 4)}, JS ${state}\n`);
        break;
      }
    }
  }
}

{
  const bytes = dump('softclip');
  line('softClip length', `${bytes.length} bytes`, bytes.length === 6001 * 8);
  const digest = createHash('sha256').update(bytes).digest('hex');
  line('softClip 6001 points', digest.slice(0, 16) + '...',
    digest === primitives.softClip.digest);
}

{
  // The cache probe is stored as values, not a digest. Exact: no transcendental
  // is involved in *whether* a recompute happens, only in the coefficients it
  // produces -- so compare the equality relations, which are what the probe is
  // actually asserting, and the values to tolerance.
  const bytes = dump('cache');
  const got = [];
  for (let i = 0; i < 9; i++) got.push(bytes.readDoubleLE(i * 8));
  const { first, second, third, equalAfterClamp } = primitives.biquad.cacheProbe;
  const cFirst = got.slice(0, 3), cSecond = got.slice(3, 6), cThird = got.slice(6, 9);

  const eq = (a, b) => a.every((v, i) => v === b[i]);
  line('cache: raw (20,8) != raw (10,5)', 'both clamp to (40,20), both recompute',
    eq(cSecond, cThird) && (equalAfterClamp === eq(cFirst, cSecond)));
  const maxd = Math.max(
    ...cFirst.map((v, i) => Math.abs(v - first[i])),
    ...cSecond.map((v, i) => Math.abs(v - second[i])),
    ...cThird.map((v, i) => Math.abs(v - third[i])),
  );
  line('cache: coefficient values', `max |diff| ${maxd.toExponential(2)}`, maxd <= TOLERANCE);
}

// --- Tier 2: bounded --------------------------------------------------------

process.stdout.write('\nTier 2 -- sin/cos involved, bound 1e-12\n');

function compareF64(label, bytes, expectedCount, generate) {
  if (bytes.length !== expectedCount * 8) {
    line(label, `length ${bytes.length}, expected ${expectedCount * 8}`, false);
    return;
  }
  let maxDiff = 0;
  let maxAt = -1;
  let exact = 0;
  let i = 0;
  for (const want of generate()) {
    const got = bytes.readDoubleLE(i * 8);
    if (got === want) exact++;
    else {
      const d = Math.abs(got - want);
      if (d > maxDiff) { maxDiff = d; maxAt = i; }
    }
    i++;
  }
  const pct = ((exact / expectedCount) * 100).toFixed(2);
  const detail = maxDiff === 0
    ? `${expectedCount} values, all bit-identical`
    : `max |diff| ${maxDiff.toExponential(2)} at ${maxAt}; ${pct}% bit-identical`;
  line(label, detail, maxDiff <= TOLERANCE);
}

compareF64('glottalPulse 101x1000 grid', dump('pulse'), 101 * 1000, function* () {
  for (let e = 0; e <= 100; e++) {
    for (let p = 0; p < 1000; p++) yield glottalPulse(p / 1000, e / 100);
  }
});

compareF64('biquad coefficient grid', dump('biquad'), 4 * 14 * 10 * 5, function* () {
  const rates = [8000, 22050, 44100, 48000];
  const bws = [0, 10, 19, 20, 21, 50, 100, 200, 400, 1000];
  for (const sr of rates) {
    const fs = [0, 10, 39, 40, 41, 100, 500, 1500, 2500, 3500, sr * 0.44, sr * 0.45, sr * 0.46, sr];
    for (const f of fs) {
      for (const bw of bws) {
        const b = new BandpassBiquad();
        b.setFreq(f, bw, sr);
        yield b.b0; yield b.b1; yield b.b2; yield b.a1; yield b.a2;
      }
    }
  }
});

process.stdout.write('\n');
if (failures) {
  process.stderr.write(`stage 1: ${failures} check(s) failed\n`);
  process.exit(1);
}
process.stdout.write('stage 1: all checks passed\n');
