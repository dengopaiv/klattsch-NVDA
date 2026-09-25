#!/usr/bin/env node
// Exit test for the text front end.
//
//   node tools/verify-text.mjs <kl_text_dump> [--cmu DIR] [--update]
//
// Three checks, each a diff or a digest, none a judgement:
//
//   1. The lift. Pass 1 of csrc/kl_text.c against Votraxxion's own ttv.c, as
//      captured in goldens/text-nrl.json by tools/capture-text-nrl.mjs: every
//      hand-corpus line exactly, and every plain word of the CMU dictionary by
//      digest when the dictionary is installed. One difference is expected
//      and named: this pass keeps ";" and ":" (as ";"), which Votraxxion
//      drops, so ";" is removed from our side before comparing. Nothing else
//      is.
//   2. The output. The whole front end -- stress, contour, pauses, spelling --
//      over the hand corpus against goldens/text-source.json. The front end
//      is deterministic, so this is exact; a change to it is re-captured with
//      --update and the diff of that file is the review.
//      A short output buffer is checked too: cut at a token boundary, the
//      longest whole-token prefix that fits, and the full length returned.
//   3. The output is klattsch. Every source line is tokenized and compiled by
//      the JavaScript engine -- the reference the C compiler is verified
//      against -- and must produce no warning and no unknown token. A front
//      end that writes something the compiler silently drops would pass 2
//      and fail here.

import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { corpusLines, cmuWords } from './text-inputs.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const dump = args[0];
const opt = (name) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
};
const update = args.includes('--update');
if (!dump) {
  console.error('usage: verify-text.mjs <kl_text_dump> [--cmu DIR] [--update]');
  process.exit(2);
}

const { tokenize, compile } = await import(
  pathToFileURL(join(root, 'src', 'engine', 'sequencer.js')).href);

function run(mode, lines) {
  const r = spawnSync(dump, [mode], { input: lines.join('\n') + '\n', maxBuffer: 1 << 30 });
  if (r.status !== 0) throw new Error(`kl_text_dump ${mode} failed: ${r.stderr}`);
  return r.stdout.toString('utf8').split('\n').slice(0, lines.length);
}

let failures = 0;
const fail = (msg) => {
  failures++;
  if (failures <= 40) console.error('FAIL ' + msg);
};

// 1. The lift ---------------------------------------------------------------

const nrl = JSON.parse(readFileSync(join(root, 'goldens', 'text-nrl.json'), 'utf8'));
const corpus = corpusLines();
const ours = (lines) => run('--nrl', lines).map((l) => l.replaceAll(';', ''));

const goldenInputs = nrl.corpus.map(([input]) => input);
if (goldenInputs.join('\n') !== corpus.join('\n')) {
  fail('tools/text-corpus.txt differs from the corpus in goldens/text-nrl.json; ' +
       're-run tools/capture-text-nrl.mjs');
} else {
  const got = ours(corpus);
  nrl.corpus.forEach(([input, want], i) => {
    if (got[i] !== want) fail(`lift, corpus: ${JSON.stringify(input)}\n  votraxxion ${want}\n  here       ${got[i]}`);
  });
  console.log(`lift, corpus: ${corpus.length} lines against Votraxxion ${nrl.votraxxion.slice(0, 7)}`);
}

const cmu = await cmuWords(opt('--cmu'));
if (!cmu) {
  console.log('lift, dictionary: SKIPPED -- cmu-pronouncing-dictionary is not installed');
} else if (`cmu-pronouncing-dictionary ${cmu.version}` !== nrl.cmu.dictionary) {
  fail(`lift, dictionary: installed cmu-pronouncing-dictionary ${cmu.version}, ` +
       `golden captured from ${nrl.cmu.dictionary}`);
} else {
  const got = ours(cmu.words);
  const digest = createHash('sha256').update(got.join('\n') + '\n').digest('hex');
  if (cmu.words.length !== nrl.cmu.words || digest !== nrl.cmu.sha256) {
    fail(`lift, dictionary: ${cmu.words.length} words, digest ${digest.slice(0, 16)}; ` +
         `golden ${nrl.cmu.words} words, ${nrl.cmu.sha256.slice(0, 16)}`);
  } else {
    console.log(`lift, dictionary: ${cmu.words.length} words, digest matches`);
  }
}

// 2. The output ---------------------------------------------------------------

const SPELL = ['a', 'Z', '!', 'hello', 'A1 b2', '@#$', 'w', '', ' '];
const source = run('--source', corpus);
const spelled = run('--spell', SPELL);
const sourcePath = join(root, 'goldens', 'text-source.json');

if (update) {
  writeFileSync(sourcePath, JSON.stringify({
    note: 'kl_text_dump --source over tools/text-corpus.txt, and --spell. ' +
          'Written by tools/verify-text.mjs --update; review the diff.',
    source: corpus.map((input, i) => [input, source[i]]),
    spell: SPELL.map((input, i) => [input, spelled[i]]),
  }, null, 1) + '\n');
  console.log(`wrote ${sourcePath}`);
} else {
  const golden = JSON.parse(readFileSync(sourcePath, 'utf8'));
  const check = (what, pairs, got) => {
    if (pairs.length !== got.length) fail(`${what}: ${got.length} lines, golden ${pairs.length}`);
    pairs.forEach(([input, want], i) => {
      if (got[i] !== want) fail(`${what}: ${JSON.stringify(input)}\n  golden ${want}\n  here   ${got[i]}`);
    });
  };
  if (golden.source.map(([i]) => i).join('\n') !== corpus.join('\n')) {
    fail('tools/text-corpus.txt differs from goldens/text-source.json; re-run with --update');
  } else {
    check('source', golden.source, source);
  }
  check('spell', golden.spell, spelled);
  console.log(`output: ${corpus.length} source lines and ${SPELL.length} spellings compared`);
}

// A short buffer: the caller learns the full length, and what was written is
// the longest prefix of whole tokens that fits with its terminator.
{
  let checked = 0;
  for (const cap of [0, 1, 2, 5, 16, 40, 100]) {
    const r = spawnSync(dump, ['--source', '--cap', String(cap)],
                        { input: corpus.join('\n') + '\n', maxBuffer: 1 << 30 });
    const lines = r.stdout.toString('utf8').split('\n').slice(0, corpus.length);
    lines.forEach((line, i) => {
      const full = source[i];
      const sp = line.indexOf(' ');
      const len = Number(sp < 0 ? line : line.slice(0, sp));
      const got = sp < 0 ? '' : line.slice(sp + 1);
      // Byte length: the C counts bytes, and the output is ASCII.
      if (len !== Buffer.byteLength(full)) fail(`cap ${cap}: ${JSON.stringify(corpus[i])} returned ${len}, full length ${Buffer.byteLength(full)}`);
      let want = '';
      if (cap > 0) {
        for (const tok of full.split(' ').filter(Boolean)) {
          const next = want ? `${want} ${tok}` : tok;
          if (next.length + 1 > cap) break;
          want = next;
        }
      }
      if (got !== want) fail(`cap ${cap}: ${JSON.stringify(corpus[i])}\n  want ${JSON.stringify(want)}\n  got  ${JSON.stringify(got)}`);
      checked++;
    });
  }
  console.log(`short buffers: ${checked} cuts checked`);
}

// 3. The output is klattsch ---------------------------------------------------

let compiled = 0;
for (const [input, src] of [...corpus.map((c, i) => [c, source[i]]),
                            ...SPELL.map((c, i) => [c, spelled[i]])]) {
  const parsed = tokenize(src);
  const unknown = parsed.tokens.filter((t) => t.type === 'unknown');
  const result = compile(parsed);
  if (unknown.length) fail(`not klattsch: ${JSON.stringify(input)} -> unknown ${unknown.map((t) => t.text).join(' ')}`);
  if (result.warnings.length) fail(`compiler warns: ${JSON.stringify(input)} -> ${result.warnings.join('; ')}`);
  compiled++;
}
console.log(`klattsch: ${compiled} outputs tokenized and compiled by the JavaScript engine`);

if (failures) {
  console.error(`${failures} failure(s)`);
  process.exit(1);
}
console.log('text front end: all checks pass');
