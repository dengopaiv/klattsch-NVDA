#!/usr/bin/env node
// How good is the text front end's pronunciation, as a number?
//
//   node tools/measure-text.mjs <kl_text_dump> [--cmu DIR] [--check|--update]
//        [--misses]
//
// Every plain word in the CMU Pronouncing Dictionary goes through
// `kl_text_dump --word`, and three things are counted against the dictionary:
//
//   words    the phonemes, stress ignored, equal to one of the dictionary's
//            pronunciations of the word
//   phones   1 - (edit distance / reference length), summed over all words,
//            against the closest pronunciation
//   stress   of the words of two or more syllables where our vowel count
//            equals the dictionary's (so the letter-to-sound error is out of
//            the way), how often our stressed vowel is the dictionary's
//            primary stress. Two reference points are counted on the same
//            words: "always the first vowel" and "the first full vowel", so
//            the stress pass is judged against doing nothing clever.
//
// The dictionary is read from the `cmu-pronouncing-dictionary` npm package,
// the optional dependency upstream's pronounce.js already names; it is used
// here to measure and is not shipped. --cmu points at a directory holding
// node_modules/ if it is not installed in this repository.
//
// --check compares the counts with goldens/text/accuracy.json and fails on any
// difference, in either direction: the front end is deterministic, so a moved
// number means a changed rule, and a changed rule is re-measured on purpose
// with --update, never absorbed silently.

import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { cmuWords, vocabulary as loadVocabulary } from './text-inputs.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const dump = args[0];
const opt = (name) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
};
const mode = args.includes('--check') ? 'check' : args.includes('--update') ? 'update' : 'report';
const goldenPath = join(root, 'goldens', 'text', 'accuracy.json');

if (!dump) {
  console.error('usage: measure-text.mjs <kl_text_dump> [--cmu DIR] [--check|--update] [--misses]');
  process.exit(2);
}

const loaded = await cmuWords(opt('--cmu'));
if (!loaded) {
  console.error('cmu-pronouncing-dictionary not found (npm install --no-save ' +
                'cmu-pronouncing-dictionary@3.0.0, or pass --cmu DIR)');
  process.exit(mode === 'check' ? 77 : 2);   // 77: ctest's "skipped"
}
const { refs } = loaded;
const vocabulary = loadVocabulary(opt('--cmu'));

const VOWELS = new Set(['AA', 'AE', 'AH', 'AO', 'AW', 'AY', 'EH', 'ER', 'EY',
                        'IH', 'IY', 'OW', 'OY', 'UH', 'UW']);

const allWords = loaded.words;

const run = spawnSync(dump, ['--word'], {
  input: allWords.join('\n') + '\n',
  maxBuffer: 1 << 30,
});
if (run.status !== 0) {
  console.error(`kl_text_dump failed: ${run.stderr}`);
  process.exit(1);
}
const lines = run.stdout.toString('utf8').split('\n');
const outputOf = new Map(allWords.map((w, i) => [w, lines[i] || '']));

function editDistance(a, b) {
  const d = Array.from({ length: a.length + 1 }, (_, i) => [i]);
  for (let j = 1; j <= b.length; j++) d[0][j] = j;
  for (let i = 1; i <= a.length; i++) {
    for (let j = 1; j <= b.length; j++) {
      d[i][j] = Math.min(d[i - 1][j] + 1, d[i][j - 1] + 1,
                         d[i - 1][j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
    }
  }
  return d[a.length][b.length];
}

function measure(words, misses) {
  const counts = {
    words: words.length,
    wordsCorrect: 0,
    phoneRefTotal: 0,
    phoneErrors: 0,
    stressEligible: 0,
    stressCorrect: 0,
    stressFirstVowel: 0,
    stressNone: 0,
  };
  words.forEach((word, i) => {
    const ours = outputOf.get(word).trim().split(/\s+/).filter(Boolean);
    const plain = ours.map((p) => p.replace(/'$/, ''));
    const variants = refs.get(word);

    let best = null;
    for (const v of variants) {
      const bare = v.map((p) => p.replace(/\d$/, ''));
      const dist = editDistance(plain, bare);
      if (!best || dist < best.dist) best = { dist, bare, v };
    }
    counts.phoneRefTotal += best.bare.length;
    counts.phoneErrors += best.dist;
    if (best.dist === 0) counts.wordsCorrect++;

    // Stress: our vowels against a variant with the same number of vowels.
    const ourVowels = plain.map((p, k) => (VOWELS.has(p) ? k : -1)).filter((k) => k >= 0);
    if (ourVowels.length < 2) return;
    const ref = variants.find((v) =>
      v.filter((p) => VOWELS.has(p.replace(/\d$/, ''))).length === ourVowels.length);
    if (!ref) return;
    const refVowels = ref.filter((p) => VOWELS.has(p.replace(/\d$/, '')));
    const primary = refVowels.findIndex((p) => p.endsWith('1'));
    if (primary < 0 || refVowels.filter((p) => p.endsWith('1')).length !== 1) return;

    counts.stressEligible++;
    const stressed = ourVowels.findIndex((k) => ours[k].endsWith("'"));
    if (stressed < 0) counts.stressNone++;
    if (stressed === primary) counts.stressCorrect++;
    else if (misses && misses.length < 80 && i % 23 === 0)
      misses.push(`${word}: ${ours.join(' ')}  |  ${ref.join(' ')}`);
    if (primary === 0) counts.stressFirstVowel++;
  });
  return counts;
}

const pct = (a, b) => (100 * a / b).toFixed(2) + '%';
function report(name, c) {
  console.log(`${name}`);
  console.log(`  words              ${c.words}`);
  console.log(`  words exact        ${c.wordsCorrect}  (${pct(c.wordsCorrect, c.words)})`);
  console.log(`  phone accuracy     ${pct(c.phoneRefTotal - c.phoneErrors, c.phoneRefTotal)}`);
  console.log(`  stress, eligible   ${c.stressEligible}`);
  console.log(`  stress, ours       ${c.stressCorrect}  (${pct(c.stressCorrect, c.stressEligible)})`);
  console.log(`  stress, 1st vowel  ${c.stressFirstVowel}  (${pct(c.stressFirstVowel, c.stressEligible)})`);
  console.log(`  stress, none given ${c.stressNone}`);
}

const misses = [];
const summary = {
  dictionary: `cmu-pronouncing-dictionary ${loaded.version}`,
  vocabulary: vocabulary ? `dictionary-en ${vocabulary.version}` : null,
  counts: { all: measure(allWords, null) },
};
if (vocabulary) {
  summary.counts.vocabulary =
    measure(allWords.filter((w) => vocabulary.stems.has(w)), misses);
}
report('all CMU words', summary.counts.all);
if (vocabulary) report('vocabulary (also in Hunspell en_US)', summary.counts.vocabulary);
if (args.includes('--misses')) for (const m of misses) console.log('  ' + m);

if (mode === 'update') {
  writeFileSync(goldenPath, JSON.stringify(summary, null, 2) + '\n');
  console.log(`wrote ${goldenPath}`);
} else if (mode === 'check') {
  if (!existsSync(goldenPath)) {
    console.error('no goldens/text/accuracy.json; run with --update');
    process.exit(1);
  }
  const golden = JSON.parse(readFileSync(goldenPath, 'utf8'));
  let bad = false;
  for (const key of ['dictionary', 'vocabulary']) {
    if (golden[key] !== summary[key]) {
      console.error(`${key} differs: golden ${golden[key]}, here ${summary[key]}`);
      bad = true;
    }
  }
  for (const set of Object.keys(golden.counts)) {
    for (const k of Object.keys(golden.counts[set])) {
      const now = summary.counts[set] && summary.counts[set][k];
      if (golden.counts[set][k] !== now) {
        console.error(`MOVED ${set}.${k}: golden ${golden.counts[set][k]}, now ${now}`);
        bad = true;
      }
    }
  }
  if (bad) process.exit(1);
  console.log('text accuracy: every count matches goldens/text/accuracy.json');
}
