// Stage 3 exit test: csrc/kl_synth.c against the JavaScript sample loop, over
// every schedule in the corpus. See docs/15-stage3-synth.md.
//
//   node tools/verify-stage3.mjs <path-to-kl_synth_dump> [--rate 48000]
//   node tools/verify-stage3.mjs <dir-of-dumps>           (files named <rate>.bin)
//
// This is the first stage where Tier 2 applies to whole rendered utterances
// rather than a grid of one function, and the first test of the "zero
// differing samples after 16-bit quantization" half of the criterion.
//
// A note on what is being compared. The JavaScript renders into a
// Float32Array, so the rounding to single precision is part of the algorithm
// and the reference samples are float32 values. The C stores to float for the
// same reason. So the comparison is between two float32 streams, and the
// meaningful question is how many samples differ at all -- not how they differ
// in float64, which neither side ever produces.

import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { renderToBuffer } from '../src/engine/index.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');

const target = process.argv[2];
if (!target || !existsSync(target)) {
  process.stderr.write(
    'usage: node tools/verify-stage3.mjs <path-to-kl_synth_dump> [--rate N]\n'
    + '       node tools/verify-stage3.mjs <dir-of-dumps>\n');
  process.exit(2);
}
const fromDir = statSync(target).isDirectory();
const rateArg = process.argv.indexOf('--rate');
const chunkArg = process.argv.indexOf('--chunked');

const manifest = JSON.parse(readFileSync(join(root, 'goldens', 'manifest.json'), 'utf8'));
const schedules = JSON.parse(readFileSync(join(root, 'goldens', 'schedules.json'), 'utf8'));
const cases = JSON.parse(readFileSync(join(root, 'goldens', 'cases.json'), 'utf8'));
const totalMsById = new Map(cases.map((c) => [c.id, c.compile.totalMs]));

const rates = rateArg > 0 ? [Number(process.argv[rateArg + 1])] : manifest.audioRates;
const binPath = join(root, 'goldens', 'schedules.bin');

let failures = 0;
const line = (label, detail, ok) => {
  process.stdout.write(`  ${label.padEnd(30)} ${ok ? 'ok' : 'FAIL'}   ${detail}\n`);
  if (!ok) failures++;
};

function dumpFor(rate) {
  if (fromDir) return readFileSync(join(target, `${rate}.bin`));
  const args = [binPath, String(rate)];
  if (chunkArg > 0) args.push('--chunked', process.argv[chunkArg + 1]);
  return execFileSync(target, args, { maxBuffer: 1 << 29 });
}

class Reader {
  constructor(b) { this.b = b; this.o = 0; }
  u32() { const v = this.b.readUInt32LE(this.o); this.o += 4; return v; }
  str() { const n = this.u32(); const s = this.b.toString('utf8', this.o, this.o + n); this.o += n; return s; }
  f32(i) { return this.b.readFloatLE(this.o + i * 4); }
  skip(n) { this.o += n; }
  get done() { return this.o === this.b.length; }
}

// Quantize exactly as wav.js does, minus the peak normalization: that is an
// encoder concern and would hide a difference behind a shared scale factor.
const q16 = (x) => {
  let s = x;
  if (s > 1) s = 1; else if (s < -1) s = -1;
  return Math.round(s * 32767);
};

for (const rate of rates) {
  process.stdout.write(`\nSample rate ${rate}\n`);
  const r = new Reader(dumpFor(rate));
  const count = r.u32();
  const ids = Object.keys(schedules).sort();

  let cmpCases = 0;
  let totalSamples = 0;
  let identicalSamples = 0;
  let maxAbsDiff = 0;
  let maxAbsAt = null;
  let q16Differing = 0;
  let q16MaxDiff = 0;
  let worstCase = null;
  let countOk = count === ids.length;

  for (let i = 0; i < count; i++) {
    const id = r.str();
    const n = r.u32();
    const expectedId = ids[i];
    if (id !== expectedId) { line(`case order at ${i}`, `${id} != ${expectedId}`, false); break; }

    const js = renderToBuffer({
      sampleRate: rate,
      schedule: schedules[id][0],
      totalMs: totalMsById.get(id),
    });

    if (js.length !== n) {
      line(`${id} length`, `C ${n} != JS ${js.length}`, false);
      r.skip(n * 4);
      continue;
    }

    let caseDiff = 0;
    for (let k = 0; k < n; k++) {
      const c = r.f32(k);
      const j = js[k];
      totalSamples++;
      if (c === j) { identicalSamples++; continue; }
      const d = Math.abs(c - j);
      if (d > maxAbsDiff) { maxAbsDiff = d; maxAbsAt = `${id}[${k}]`; }
      if (d > caseDiff) caseDiff = d;
      const qc = q16(c), qj = q16(j);
      if (qc !== qj) {
        q16Differing++;
        const qd = Math.abs(qc - qj);
        if (qd > q16MaxDiff) q16MaxDiff = qd;
      }
    }
    if (caseDiff > 0 && (!worstCase || caseDiff > worstCase.d)) worstCase = { id, d: caseDiff };
    r.skip(n * 4);
    cmpCases++;
  }

  line('case count', `${count}`, countOk);
  line('stream consumed', `${r.o} bytes`, r.done);
  line('cases compared', `${cmpCases}`, cmpCases === ids.length);

  const pct = totalSamples ? ((identicalSamples / totalSamples) * 100) : 0;
  line('samples bit-identical',
    `${identicalSamples}/${totalSamples} (${pct.toFixed(4)}%)`,
    true);
  line('peak float32 difference',
    maxAbsDiff === 0 ? 'none' : `${maxAbsDiff.toExponential(3)} at ${maxAbsAt}`,
    true);
  if (worstCase) process.stdout.write(`      worst case: ${worstCase.id} (${worstCase.d.toExponential(2)})\n`);

  // The actual bar.
  line('16-bit differing samples', `${q16Differing}` + (q16MaxDiff ? ` (max ${q16MaxDiff} LSB)` : ''),
    q16Differing === 0);
}

process.stdout.write('\n');
if (failures) {
  process.stderr.write(`stage 3: ${failures} check(s) failed\n`);
  process.exit(1);
}
process.stdout.write('stage 3: all checks passed\n');
