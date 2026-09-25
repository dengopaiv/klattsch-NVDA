// The inputs the text front end is checked against, in one place, so that
// the capture, the verifier and the measurement cannot disagree about them.
//
//   corpusLines()      tools/text-corpus.txt, comments and blanks dropped
//   cmuWords(dir)      every plain word of the CMU Pronouncing Dictionary,
//                      sorted, with its pronunciations; null if the optional
//                      `cmu-pronouncing-dictionary` package is not installed
//   vocabulary(dir)    the lower-case stems of the Hunspell en_US word list
//                      (`dictionary-en`); null if not installed
//
// Both packages are looked for beside `dir` (a directory with node_modules/),
// then KL_CMU_DIR, then this repository. They are measurement inputs, never
// shipped and never checked in.

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');

export function corpusLines() {
  return readFileSync(join(root, 'tools', 'text-corpus.txt'), 'utf8')
    .split(/\r?\n/)
    .filter((l) => l !== '' && !l.startsWith('#'));
}

function packageDir(name, dir) {
  for (const base of [dir, process.env.KL_CMU_DIR, root].filter(Boolean)) {
    try {
      const req = createRequire(join(resolve(base), 'package.json'));
      // Packages that export only their entry point still keep their
      // package.json and data files beside it.
      return dirname(req.resolve(name));
    } catch { /* try the next */ }
  }
  return null;
}

const versionOf = (dir) => JSON.parse(readFileSync(join(dir, 'package.json'), 'utf8')).version;

export async function cmuWords(dir) {
  const pkg = packageDir('cmu-pronouncing-dictionary', dir);
  if (!pkg) return null;
  const entry = createRequire(join(pkg, 'package.json')).resolve('cmu-pronouncing-dictionary');
  const { dictionary } = await import(pathToFileURL(entry).href);
  const refs = new Map();
  for (const [key, value] of Object.entries(dictionary)) {
    const word = key.replace(/\(\d+\)$/, '');
    if (!/^[a-z]+('[a-z]+)?$/.test(word)) continue;
    if (!refs.has(word)) refs.set(word, []);
    refs.get(word).push(value.trim().split(/\s+/));
  }
  return { words: [...refs.keys()].sort(), refs, version: versionOf(pkg) };
}

export function vocabulary(dir) {
  const pkg = packageDir('dictionary-en', dir);
  if (!pkg) return null;
  const stems = new Set(readFileSync(join(pkg, 'index.dic'), 'utf8')
    .split(/\r?\n/).slice(1)
    .map((l) => l.split('/')[0])
    .filter((w) => /^[a-z]+('[a-z]+)?$/.test(w)));
  return { stems, version: versionOf(pkg) };
}
