// Stage 6 exit test: csrc/kl_wav.c, csrc/kl_render.c and bin/klattsch_cli.c
// against the JavaScript they came from. See docs/18-stage6-wav.md.
//
//   node tools/verify-stage6.mjs <kl_wav_dump> [--cli <klattsch>]
//   node tools/verify-stage6.mjs <directory-of-dumps>
//   node tools/verify-stage6.mjs --list-cli-cases
//
// Whole files, byte for byte, at all three sample rates, over the whole
// corpus. There is no tolerance here and there cannot be one: the file is
// 16-bit, and normalization makes the gain a function of the single loudest
// sample of the mix, so one differing sample either changes nothing at all or
// changes every byte after the header.
//
// Six things are checked, in the order a failure is easiest to read in:
//
//   1. the reference has not moved -- bin/klattsch.mjs still does the six
//      steps this file reproduces, and still writes the ISFT field the C
//      carries. A verifier that reproduces a stale pipeline is worse than no
//      verifier, and the ISFT field is upstream's credit in every file the
//      engine produces.
//   2. Math.round, swept. It is the one function in kl_wav.c that has no C
//      library equivalent -- round() breaks ties away from zero and
//      floor(x + 0.5) is wrong at 0.49999999999999994 -- so it is measured
//      rather than argued for.
//   3. kl_to_fixed against toFixed, for the same reason: printf rounds an
//      exact tie to even and toFixed rounds it away from zero, and both of
//      the CLI's ties are reachable.
//   4. the encoder goldens, which reach what the corpus cannot: no metadata,
//      normalization off, the clamp, an odd-length comment, an empty file.
//   5. the corpus: 729 cases at 48000, 22050 and 8000 Hz, whole files.
//   6. the CLI itself, end to end -- both real programs run on the same
//      text, and the files and the stderr lines compared.
//
// (5) also reports a number that is not a pass or a fail: how many samples
// sit close enough to a 16-bit rounding boundary that a one-ULP float32
// disagreement would flip them. Stage 3 measured four such disagreements in
// 2.9 million samples; this measures how much room they have. It is the
// quantity that would have to move for this stage's byte-identity to stop
// holding, so it is watched rather than assumed.

import { createHash } from 'node:crypto';
import { execFileSync, spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { compile, tokenize } from '../src/engine/sequencer.js';
import { renderToBuffer } from '../src/engine/synth-core.js';
import { encodeWav } from '../src/engine/wav.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..');

// All three by default. `--rate N` narrows it to one, which is what the
// mutation suite and CI use: a mutation caught at 48 kHz is caught, and the
// exit test itself is the three-rate run that ctest performs.
const rateIdx = process.argv.indexOf('--rate');
const RATES = rateIdx > 0 ? [Number(process.argv[rateIdx + 1])] : [48000, 22050, 8000];

const cases = JSON.parse(readFileSync(join(root, 'goldens', 'cases.json'), 'utf8'));
const jsCli = readFileSync(join(root, 'bin', 'klattsch.mjs'), 'utf8');

// The ISFT string, taken from the JavaScript rather than written out again
// here, so that this file cannot be the place the two drift apart.
const SOFTWARE = (() => {
  const m = jsCli.match(/software: '([^']*)'/);
  if (!m) {
    process.stderr.write('bin/klattsch.mjs no longer has a software: field\n');
    process.exit(2);
  }
  return m[1];
})();

// ---------------------------------------------------------------------------
// The mix, reproduced from bin/klattsch.mjs
//
// Not imported, because there is nothing to import: upstream's CLI is a
// script, and these six lines are the only place it puts them. Section 1
// below is what keeps this copy honest, and section 5 runs the real program.
// ---------------------------------------------------------------------------

function renderCli(text, opts, sampleRate) {
  const { voices, totalMs } = compile(tokenize(text), opts ?? {});
  const buf = new Float32Array(Math.ceil(totalMs * sampleRate / 1000));
  for (const v of voices) {
    if (!v.schedule.length) continue;
    const vb = renderToBuffer({ sampleRate, schedule: v.schedule, totalMs: v.totalMs });
    const n = Math.min(buf.length, vb.length);
    for (let i = 0; i < n; i++) buf[i] += vb[i];
  }
  return { buf, totalMs };
}

function encodeCli(buf, sampleRate, text) {
  return encodeWav(buf, sampleRate, {
    metadata: { software: SOFTWARE, comment: text },
  });
}

// The end-to-end set: the texts both real programs are run on.
let cliCases = null;
function pickCliCases() {
  if (cliCases) return cliCases;
  const picked = [];
  const take = (text) => { if (text != null && !picked.includes(text)) picked.push(text); };

  take('HH AH L OW');
  take('b140 AY+30 . AY-30');
  // Named because each reaches something the first two do not: a syllable
  // group, two voice sections to mix, a warning on stderr, a bank chosen from
  // inside the text, and two non-ASCII arguments -- which on Windows only
  // survive because the CLI reads its arguments as UTF-16 and converts.
  for (const id of ['syllable/many', 'voice/uneven-lengths', 'unknown/several',
                    'utterance/japanese', 'normalize/nfkc-fullwidth',
                    'normalize/cyrillic', 'syllable/unclosed']) {
    const c = cases.find((x) => x.id === id);
    if (c && (!c.opts || Object.keys(c.opts).length === 0)) take(c.text);
  }

  // The two toFixed ties, which are where printf and JavaScript actually
  // disagree: an exact tie whose lower neighbour is even, so ties-to-even
  // rounds down where toFixed rounds the magnitude up.
  //
  //  * kilobytes. Measured over the 460 cases the CLI can be driven with:
  //    three have a file of exactly 25,088 bytes, which is 24.5 KB, where
  //    printf prints 24 and JavaScript prints 25. `syllable/unclosed` is one
  //    of them and is in the list above, so this tie needs nothing built.
  //  * seconds. Measured the same way: *no* corpus case makes totalMs/1000
  //    an exact tie at two decimals, so this one is constructed.
  //    "[rate=175] AA ." compiles to exactly 625 ms, and (0.625).toFixed(2)
  //    is "0.63" where printf("%.2f") gives "0.62".
  take('[rate=175] AA .');

  cliCases = picked;
  return picked;
}

if (process.argv[2] === '--list-cli-cases') {
  for (const t of pickCliCases()) process.stdout.write(t + '\n');
  process.exit(0);
}

const dumpTool = process.argv[2];
if (!dumpTool || !existsSync(dumpTool)) {
  process.stderr.write('usage: node tools/verify-stage6.mjs <kl_wav_dump> [--cli <klattsch>]\n');
  process.exit(2);
}
const fromDir = statSync(dumpTool).isDirectory();
const cliIdx = process.argv.indexOf('--cli');
// Resolved, because the CLI is run with its cwd in a temp directory so that
// both programs are given the same relative output path and print the same
// line. A relative tool path would stop resolving there.
const cliTool = cliIdx > 0 ? resolve(process.argv[cliIdx + 1]) : null;

const capture = (file, args) =>
  fromDir ? readFileSync(join(dumpTool, file))
          : execFileSync(dumpTool, args, { maxBuffer: 1 << 30 });

let failures = 0;
const line = (label, detail, ok) => {
  process.stdout.write(`  ${label.padEnd(42)} ${ok ? 'ok  ' : 'FAIL'}   ${detail}\n`);
  if (!ok) failures++;
};
const note = (label, detail) => {
  process.stdout.write(`  ${label.padEnd(42)} --     ${detail}\n`);
};
const show = (problems, n = 10) => {
  for (const p of problems.slice(0, n)) process.stdout.write(`      ${p}\n`);
  if (problems.length > n) process.stdout.write(`      ... and ${problems.length - n} more\n`);
};

class Reader {
  constructor(b) { this.b = b; this.o = 0; }
  u32() { const v = this.b.readUInt32LE(this.o); this.o += 4; return v; }
  f64() { const v = this.b.readDoubleLE(this.o); this.o += 8; return v; }
  bytes(n) { const v = this.b.subarray(this.o, this.o + n); this.o += n; return v; }
  get done() { return this.o === this.b.length; }
}

const sha = (b) => createHash('sha256').update(b).digest('hex');

// The gap between a float32 and its neighbour, exactly. Used to ask how much
// room a sample has before a one-ULP disagreement would change its 16-bit
// value -- the quantity stage 3's four differing samples have to stay inside.
const f32 = new Float32Array(1);
const u32v = new Uint32Array(f32.buffer);
function ulp32(v) {
  f32[0] = Math.abs(v);
  const a = f32[0];
  const bits = u32v[0];
  u32v[0] = bits + 1;
  const next = f32[0];
  u32v[0] = bits;
  return next - a;
}
const asBuffer = (u8) => Buffer.from(u8.buffer, u8.byteOffset, u8.length);

// Where two WAV files differ, in words rather than in hex.
function locate(a, b) {
  const n = Math.min(a.length, b.length);
  let i = 0;
  while (i < n && a[i] === b[i]) i++;
  if (i === n && a.length === b.length) return null;
  if (i === n) return `identical for ${n} bytes, then lengths differ (${a.length} vs ${b.length})`;
  if (i < 44) return `header byte ${i}: ${a[i]} vs ${b[i]}`;
  const s = (i - 44) >> 1;
  if (44 + s * 2 + 1 < a.length && 44 + s * 2 + 1 < b.length) {
    const av = a.readInt16LE(44 + s * 2), bv = b.readInt16LE(44 + s * 2);
    return `sample ${s}: ${av} vs ${bv} (first differing byte ${i})`;
  }
  return `byte ${i}: ${a[i]} vs ${b[i]}`;
}

process.stdout.write('\nstage 6: the WAV encoder, the mix, and the CLI\n');

// ---------------------------------------------------------------------------
// 1. The reference has not moved
// ---------------------------------------------------------------------------

process.stdout.write('\n  the reference bin/klattsch.mjs\n');
{
  // Every step this file reproduces, each required to appear exactly once.
  // If upstream's CLI is ever edited, this fails and the copy above has to be
  // brought back into line -- which is the whole point of not importing it.
  const steps = [
    'const sampleRate = 48000;',
    'compileString(text)',
    'new Float32Array(Math.ceil(totalMs * sampleRate / 1000))',
    'if (!v.schedule.length) continue;',
    'renderToBuffer({ sampleRate, schedule: v.schedule, totalMs: v.totalMs })',
    'Math.min(buf.length, vb.length)',
    'buf[i] += vb[i];',
    'encodeWav(buf, sampleRate, {',
    'comment: text,',
    "outPath = 'klattsch.wav'",
  ];
  const wrong = [];
  for (const s of steps) {
    const n = jsCli.split(s).length - 1;
    if (n !== 1) wrong.push(`${JSON.stringify(s)} appears ${n} times, expected 1`);
  }
  line("the CLI's pipeline is unchanged", `${steps.length} fragments`, wrong.length === 0);
  show(wrong);

  // The attribution, compared against the C constant rather than against a
  // copy of the string in this file.
  const header = readFileSync(join(root, 'csrc', 'kl_wav.h'), 'utf8');
  const m = header.match(/#define KL_WAV_SOFTWARE "([^"]*)"/);
  const decoded = m
    ? Buffer.from(m[1].replace(/\\x([0-9A-Fa-f]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16))), 'latin1').toString('utf8')
    : null;
  line('KL_WAV_SOFTWARE matches the JS', JSON.stringify(SOFTWARE), decoded === SOFTWARE);
  line('the CLI writes it', 'bin/klattsch_cli.c uses KL_WAV_SOFTWARE',
       readFileSync(join(root, 'bin', 'klattsch_cli.c'), 'utf8').includes('meta.software = KL_WAV_SOFTWARE'));
}

// ---------------------------------------------------------------------------
// 2. Math.round, swept
// ---------------------------------------------------------------------------

process.stdout.write('\n  kl_wav_round against Math.round\n');
{
  const r = new Reader(capture('round-sweep.bin', ['--round-sweep']));
  const n = r.u32();
  const problems = [];
  let ties = 0, negZero = 0;
  for (let i = 0; i < n; i++) {
    const x = r.f64(), y = r.f64();
    const want = Math.round(x);
    if (!Object.is(y, want) && !(Number.isNaN(y) && Number.isNaN(want))) {
      problems.push(`Math.round(${x}) = ${want}, C gave ${y}`);
    }
    if (Number.isFinite(x) && Math.abs(x % 1) === 0.5) ties++;
    if (Object.is(want, -0)) negZero++;
  }
  line('every value identical', `${n} values, ${ties} exact ties, ${negZero} give -0`,
       problems.length === 0 && r.done);
  show(problems);
}

// ---------------------------------------------------------------------------
// 3. kl_to_fixed against Number.prototype.toFixed
// ---------------------------------------------------------------------------

process.stdout.write('\n  kl_to_fixed against toFixed\n');
{
  const r = new Reader(capture('tofixed-sweep.bin', ['--tofixed-sweep']));
  const n = r.u32();
  const problems = [];
  let ties = 0;
  for (let i = 0; i < n; i++) {
    const x = r.f64();
    const digits = r.u32();
    const got = r.bytes(r.u32()).toString('utf8');
    const want = x.toFixed(digits);
    if (got !== want) problems.push(`(${x}).toFixed(${digits}) = "${want}", C gave "${got}"`);
    const scaled = x * 2 ** (digits + 1);
    if (Number.isInteger(scaled) && Math.abs(scaled % 2) === 1) ties++;
  }
  line('every value identical', `${n} values, ${ties} exact ties`, problems.length === 0 && r.done);
  show(problems);
}

// ---------------------------------------------------------------------------
// 4. The encoder goldens
// ---------------------------------------------------------------------------

process.stdout.write('\n  the encoder goldens\n');
{
  const golden = JSON.parse(readFileSync(join(root, 'goldens', 'wav.json'), 'utf8'));
  const order = ['plain', 'withMeta', 'noNormalize', 'oddComment', 'clipped', 'empty',
                 'emptySoftware'];
  const r = new Reader(capture('wav-goldens.bin', ['--wav-goldens']));
  const n = r.u32();
  const problems = [];
  let ok = 0;
  if (n !== order.length) problems.push(`${n} encodings, expected ${order.length}`);
  for (let i = 0; i < n && i < order.length; i++) {
    const name = order[i];
    const len = r.u32(), gain = r.f64();
    const bytes = r.bytes(len);
    const g = golden[name];
    let bad = null;
    if (len !== g.bytes) bad = `length ${len}, golden ${g.bytes}`;
    else if (sha(bytes) !== g.digest) bad = `digest ${sha(bytes).slice(0, 16)}, golden ${g.digest.slice(0, 16)}`;
    else if (g.gain !== undefined && !Object.is(gain, g.gain)) bad = `gain ${gain}, golden ${g.gain}`;
    if (bad) problems.push(`${name}: ${bad}`); else ok++;
  }
  line('every encoding matches the golden', `${ok}/${order.length}`, ok === order.length && r.done);
  show(problems);
}

// ---------------------------------------------------------------------------
// 5. The corpus, at every rate, whole files
// ---------------------------------------------------------------------------

for (const rate of RATES) {
  process.stdout.write(`\n  the corpus at ${rate} Hz\n`);
  const r = new Reader(capture(`wav${rate}.bin`, [join(root, 'goldens', 'cases-compile.bin'), String(rate)]));
  const n = r.u32();
  const problems = [];
  let identical = 0, gainOk = 0, totalBytes = 0, samples = 0;
  let fragile = 0, minMargin = Infinity, minMarginAt = '';

  if (n !== cases.length) problems.push(`${n} cases in the dump, corpus has ${cases.length}`);

  for (let i = 0; i < n && i < cases.length; i++) {
    const c = cases[i];
    const len = r.u32(), gain = r.f64();
    const cBytes = r.bytes(len);
    totalBytes += len;

    const { buf } = renderCli(c.text, c.opts, rate);
    const js = encodeCli(buf, rate, c.text);
    const jsBytes = asBuffer(js.bytes);
    samples += buf.length;

    if (Object.is(gain, js.gain)) gainOk++;
    else problems.push(`${c.id}: gain ${gain} vs ${js.gain}`);

    if (jsBytes.length === cBytes.length && jsBytes.compare(cBytes) === 0) identical++;
    else problems.push(`${c.id}: ${locate(jsBytes, cBytes)}`);

    // How much room the 16-bit rounding has. A sample whose scaled value sits
    // within one float32 ULP of a .5 boundary is one where a sub-ULP
    // disagreement between two libms would change the file.
    for (let k = 0; k < buf.length; k++) {
      let s = buf[k] * js.gain;
      if (s > 1) s = 1; else if (s < -1) s = -1;
      const v = s * 32767;
      const margin = Math.abs(v - Math.floor(v) - 0.5);
      const reach = ulp32(buf[k]) * Math.abs(js.gain) * 32767;
      if (margin < reach) fragile++;
      if (margin < minMargin) { minMargin = margin; minMarginAt = `${c.id}[${k}]`; }
    }
  }

  line('every file byte-identical', `${identical}/${n}`, identical === n && r.done);
  line('normalization gain exact', `${gainOk}/${n}`, gainOk === n);
  note('bytes compared', `${(totalBytes / (1024 * 1024)).toFixed(1)} MB over ${samples.toLocaleString('en-US')} samples`);
  note('closest 16-bit rounding boundary', `${minMargin.toExponential(2)} LSB at ${minMarginAt}`);
  note('samples a 1-ULP slip could flip', `${fragile} of ${samples.toLocaleString('en-US')}`);
  show(problems);
}

// ---------------------------------------------------------------------------
// 6. The CLI, end to end
// ---------------------------------------------------------------------------

process.stdout.write('\n  bin/klattsch_cli.c against bin/klattsch.mjs\n');
{
  const texts = pickCliCases();
  const problems = [];
  let same = 0, sameErr = 0, ran = 0;

  // stderr is the point of the comparison, so spawnSync rather than
  // execFileSync -- the latter returns stdout, and both programs write
  // nothing there.
  const run = (exe, args, cwd) => {
    const r = spawnSync(exe, args, { cwd, encoding: 'buffer' });
    if (r.error) throw r.error;
    if (r.status !== 0) throw new Error(`${exe} exited ${r.status}: ${r.stderr}`);
    return r.stderr.toString('utf8');
  };

  const runPair = (text, i) => {
    if (cliTool) {
      const a = mkdtempSync(join(tmpdir(), 'kl-js-'));
      const b = mkdtempSync(join(tmpdir(), 'kl-c-'));
      try {
        const ae = run(process.execPath, [join(root, 'bin', 'klattsch.mjs'), text, 'out.wav'], a);
        const be = run(cliTool, [text, 'out.wav'], b);
        return { js: readFileSync(join(a, 'out.wav')), c: readFileSync(join(b, 'out.wav')),
                 jsErr: ae, cErr: be };
      } finally {
        rmSync(a, { recursive: true, force: true });
        rmSync(b, { recursive: true, force: true });
      }
    }
    // Directory mode: the C side was run wherever the dumps were made.
    const wav = join(dumpTool, `cli-${i}.wav`);
    if (!existsSync(wav)) return null;
    const a = mkdtempSync(join(tmpdir(), 'kl-js-'));
    try {
      const ae = run(process.execPath, [join(root, 'bin', 'klattsch.mjs'), text, 'out.wav'], a);
      const errPath = join(dumpTool, `cli-${i}.err`);
      return { js: readFileSync(join(a, 'out.wav')), c: readFileSync(wav),
               jsErr: ae,
               cErr: existsSync(errPath) ? readFileSync(errPath, 'utf8') : null };
    } finally {
      rmSync(a, { recursive: true, force: true });
    }
  };

  for (let i = 0; i < texts.length; i++) {
    const got = runPair(texts[i], i);
    if (!got) continue;
    ran++;
    if (got.js.length === got.c.length && got.js.compare(got.c) === 0) same++;
    else problems.push(`${JSON.stringify(texts[i])}: ${locate(got.js, got.c)}`);
    if (got.cErr === null) sameErr++;                      // not captured on this leg
    else if (got.jsErr.replace(/\r\n/g, '\n') === got.cErr.replace(/\r\n/g, '\n')) sameErr++;
    else problems.push(`${JSON.stringify(texts[i])}: stderr ${JSON.stringify(got.jsErr)} vs ${JSON.stringify(got.cErr)}`);
  }

  // Two invocations the corpus loop cannot reach, both only meaningful when
  // the real program is here to run: the default output path, and an output
  // path that is not ASCII. The second is the only thing that distinguishes
  // _wfopen from fopen on Windows, where fopen would put a UTF-8 path through
  // the ANSI codepage and write a file under a different name.
  if (cliTool) {
    for (const [label, args, name] of [
      ['the default output path', [], 'klattsch.wav'],
      ['a non-ASCII output path', ['\u30cf\u30ed\u30fc.wav'], '\u30cf\u30ed\u30fc.wav'],
    ]) {
      const a = mkdtempSync(join(tmpdir(), 'kl-js-'));
      const b = mkdtempSync(join(tmpdir(), 'kl-c-'));
      try {
        const jsErr = run(process.execPath, [join(root, 'bin', 'klattsch.mjs'), 'HH AH L OW', ...args], a);
        const cErr = run(cliTool, ['HH AH L OW', ...args], b);
        const js = readFileSync(join(a, name));
        const c = readFileSync(join(b, name));
        ran++;
        if (js.length === c.length && js.compare(c) === 0) same++;
        else problems.push(`${label}: ${locate(js, c)}`);
        if (jsErr.replace(/\r\n/g, '\n') === cErr.replace(/\r\n/g, '\n')) sameErr++;
        else problems.push(`${label}: stderr ${JSON.stringify(jsErr)} vs ${JSON.stringify(cErr)}`);
      } catch (e) {
        ran++;
        problems.push(`${label}: ${e.message}`);
      } finally {
        rmSync(a, { recursive: true, force: true });
        rmSync(b, { recursive: true, force: true });
      }
    }
  }

  if (ran === 0) note('end-to-end comparison', 'skipped: no --cli and no cli-*.wav in the dump directory');
  else {
    line('the two programs write the same file', `${same}/${ran}`, same === ran);
    line('and print the same thing', `${sameErr}/${ran}`, sameErr === ran);
  }
  show(problems);
}

process.stdout.write('\n');
if (failures) {
  process.stderr.write(`stage 6: ${failures} check(s) failed\n`);
  process.exit(1);
}
process.stdout.write('stage 6: all checks passed\n');
