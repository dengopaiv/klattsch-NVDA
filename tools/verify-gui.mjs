#!/usr/bin/env node
// Exit test for the sample generator's engine path.
//
//   node tools/verify-gui.mjs <klattsch_gui.exe> <kl_text_dump>
//
// The generator (gui-native/klattsch_gui.cpp) has a headless mode,
// `--selftest`, that runs the code its Speak and Save buttons run -- the
// voice settings read as numbers, the text front end, tokenize, compile,
// render, mix, encode -- and writes the WAV. This drives the real executable
// through that mode for a set of cases and requires every file to be
// byte-identical to the JavaScript reference engine rendering the same
// source with the same settings, the way bin/klattsch.mjs renders.
//
// For text-mode cases the phoneme source comes from `kl_text_dump --source
// --base HZ`, the front end verified on its own in tools/verify-text.mjs, and
// is also checked against the ICMT chunk of the generator's own file: the
// generator writes the source it spoke into every WAV.
//
// What this does not check is the window: that the controls reach these
// numbers, that the tab order is right, that NVDA reads it. That is the
// keyboard-and-NVDA pass in docs/20-generator.md, done by a person.

import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const [gui, dump] = process.argv.slice(2);
if (!gui || !dump) {
  console.error('usage: verify-gui.mjs <klattsch_gui.exe> <kl_text_dump>');
  process.exit(2);
}

const { compileString, renderToBuffer, encodeWav } = await import(
  pathToFileURL(join(root, 'src', 'engine', 'index.js')).href);

// The generator's PARAMS table, in its order: [option, default, unit]. The
// unit is what turns a spin-box integer into the engine's value, and the
// multiplication is done here exactly as the C++ does it (value * unit).
const PARAMS = [
  ['baseF0', 120, 1], ['rate', 110, 1], ['scale', 100, 0.01],
  ['vibratoDepth', 0, 1], ['vibratoRate', 5, 1],
  ['tremoloDepth', 0, 0.01], ['tremoloRate', 5, 1],
  ['aspiration', 0, 0.01], ['tilt', 0, 0.01], ['effort', 50, 0.01],
];
const DEFAULTS = PARAMS.map(([, d]) => d);
const SOFTWARE = 'klattsch · https://tgies.github.io/klattsch';

const set = (i, v) => DEFAULTS.map((d, k) => (k === i ? v : d));

// Each case reaches something: the defaults in both modes, every setting off
// its default (one at a time, then all at once), every bank, every sample
// rate the combo offers, a two-voice source, a source with a warning, and
// text outside ASCII.
const CASES = [
  { name: 'defaults, phonemes', mode: 1, text: 'HH AH L OW' },
  { name: 'defaults, text', mode: 0, text: 'Hello. Is this working?' },
  ...PARAMS.map(([opt], i) => ({
    name: `${opt} off its default`, mode: 1,
    text: 'b AY' + "'" + ' S EH D , HH AH L OW .',
    params: set(i, [180, 70, 85, 6, 7, 40, 9, 30, -45, 80][i]),
  })),
  { name: 'every setting at once, text', mode: 0, text: 'Red, green, and blue!',
    params: [95, 140, 120, 3, 4, 25, 6, 15, 30, 20] },
  { name: 'text contour at a high base pitch', mode: 0, text: 'Are you sure?',
    params: set(0, 220) },
  { name: 'Japanese bank', mode: 1, bank: 'ja-mokhtari-2000', text: 'K O N N I CH I W A' },
  { name: 'second Japanese bank', mode: 1, bank: 'ja-hecko-2026', text: 'K O N N I CH I W A' },
  ...[8000, 11025, 16000, 22050, 44100].map((rate) => ({
    name: `${rate} Hz`, mode: 0, rate, text: 'The quick brown fox.',
  })),
  { name: 'two voices', mode: 1, text: 'bC3 AA r400 AA [voice=1] bC4 IY r400 IY' },
  { name: 'a warning', mode: 1, text: 'HH AH QQ L OW' },
  { name: 'text outside ASCII', mode: 0, text: '“Café,” she said — naïve.' },
];

function reference(source, params, bank, rate) {
  const opts = { bank };
  PARAMS.forEach(([opt, , unit], k) => { opts[opt] = params[k] * unit; });
  const { voices, totalMs } = compileString(source, opts);
  const buf = new Float32Array(Math.ceil(totalMs * rate / 1000));
  for (const v of voices) {
    if (!v.schedule.length) continue;
    const vb = renderToBuffer({ sampleRate: rate, schedule: v.schedule, totalMs: v.totalMs });
    const n = Math.min(buf.length, vb.length);
    for (let i = 0; i < n; i++) buf[i] += vb[i];
  }
  return Buffer.from(encodeWav(buf, rate, {
    metadata: { software: SOFTWARE, comment: source },
  }).bytes);
}

// The ICMT chunk of a WAV written by kl_wav.c: LIST, INFO, then fields.
function comment(wav) {
  const i = wav.indexOf(Buffer.from('ICMT'));
  if (i < 0) return null;
  const len = wav.readUInt32LE(i + 4);
  return wav.subarray(i + 8, i + 8 + len).toString('utf8').replace(/\0+$/, '');
}

const work = mkdtempSync(join(tmpdir(), 'kl-gui-'));
let failures = 0;
try {
  CASES.forEach((c, n) => {
    const params = c.params || DEFAULTS;
    const bank = c.bank || 'klatt1980-en';
    const rate = c.rate || 48000;
    const out = join(work, `case${n}.wav`);

    const r = spawnSync(gui, ['--selftest', out, String(c.mode), bank, String(rate),
                              ...params.map(String), c.text]);
    if (r.status !== 0) {
      console.error(`FAIL ${c.name}: --selftest exited ${r.status}`);
      failures++;
      return;
    }
    const got = readFileSync(out);

    let source = c.text;
    if (c.mode === 0) {
      const d = spawnSync(dump, ['--source', '--base', String(params[0] * PARAMS[0][2])],
                          { input: c.text + '\n' });
      source = d.stdout.toString('utf8').split('\n')[0];
      if (comment(got) !== source) {
        console.error(`FAIL ${c.name}: the WAV's ICMT is not the front end's source\n` +
                      `  ICMT   ${comment(got)}\n  source ${source}`);
        failures++;
        return;
      }
    }
    const want = reference(source, params, bank, rate);
    if (!got.equals(want)) {
      let at = 0;
      while (at < got.length && at < want.length && got[at] === want[at]) at++;
      console.error(`FAIL ${c.name}: ${got.length} bytes, reference ${want.length}; first difference at byte ${at}`);
      failures++;
      return;
    }
    console.log(`  ok  ${c.name}  (${got.length} bytes)`);
  });
} finally {
  rmSync(work, { recursive: true, force: true });
}

if (failures) {
  console.error(`${failures} of ${CASES.length} cases failed`);
  process.exit(1);
}
console.log(`generator: all ${CASES.length} cases byte-identical to the JavaScript reference`);
