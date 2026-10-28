#!/usr/bin/env node
// Exit test for the compiled-in dictionary (docs/23-dictionary.md).
//
//   node tools/verify-dict.mjs <kl_text_dump>
//
// Two checks, each a diff, none a judgement:
//
//   1. The table is the file. Every entry of data/cmudict/cmudict.dict --
//      135,166 of them, alternatives and odd keys included -- parsed here
//      independently of csrc/tools/kl_dict_gen.c, looked up through the very
//      binary under test (`kl_text_dump --lookup`), and compared phone for
//      phone. Keys that are not in the file must come back empty.
//   2. The front end uses it as decided. Hand cases, each a substring of
//      `kl_text_dump --source` with the dictionary on, or an equality with it
//      off where the dictionary must not reach: function words keep no
//      stress, digits never reach it, quoted words are found, the last of
//      two primaries wins, and a word it lacks goes to the rules unchanged.

import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const dump = process.argv[2];
if (!dump) {
  console.error('usage: verify-dict.mjs <kl_text_dump>');
  process.exit(2);
}

let failures = 0;
const fail = (msg) => {
  failures++;
  if (failures <= 40) console.error('FAIL ' + msg);
};

function run(args, lines) {
  const r = spawnSync(dump, args, { input: lines.join('\n') + '\n', maxBuffer: 1 << 30 });
  if (r.status !== 0) throw new Error(`kl_text_dump ${args.join(' ')} failed: ${r.stderr}`);
  return r.stdout.toString('utf8').split('\n').slice(0, lines.length);
}

// 1. The table is the file ----------------------------------------------------

const entries = [];
for (const raw of readFileSync(join(root, 'data', 'cmudict', 'cmudict.dict'), 'utf8').split('\n')) {
  const line = raw.replace(/\r$/, '').split(' #')[0].trim();
  if (!line) continue;
  const [key, ...phones] = line.split(/\s+/);
  entries.push([key.replace(/[a-z]/g, (c) => c.toUpperCase()), phones.join(' ')]);
}
const got = run(['--lookup'], entries.map(([k]) => k));
entries.forEach(([key, want], i) => {
  if (got[i] !== want) fail(`table: ${key}\n  file  ${want}\n  table ${got[i]}`);
});
const keys = new Set(entries.map(([k]) => k));
const absent = ['XYZZYQ', 'HELLOO', 'HELL0', 'A(9)', 'ZZZZZZZZZZZZZZZZZZZZ', "'", 'AAAAA']
  .filter((k) => !keys.has(k));
run(['--lookup'], absent).forEach((g, i) => {
  if (g !== '-') fail(`table: ${absent[i]} is not in the file, but the table gives ${g}`);
});
console.log(`table: ${entries.length} entries equal to the file; ${absent.length} absent keys absent`);

// 2. The front end uses it as decided -------------------------------------------

const on = (t) => run(['--source'], [t])[0];
const off = (t) => run(['--source', '--dict', 'off'], [t])[0];
// The phones and stress marks alone: the contour's `b` steps and pitch deltas
// are written between and onto phones, and would split what is looked for.
const phones = (s) => s.split(' ')
  .filter((t) => !/^b([+-][\d.]+)?$/.test(t))
  .map((t) => t.replace(/\([+-][\d.]+\)$|[+-][\d.]+$/, ''))
  .join(' ');
const has = (what, text, sub) => {
  const s = phones(on(text));
  if (!s.includes(sub)) fail(`${what}: ${JSON.stringify(text)} -> ${s}\n  expected to contain ${sub}`);
};
const lacks = (what, text, sub) => {
  const s = phones(on(text));
  if (s.includes(sub)) fail(`${what}: ${JSON.stringify(text)} -> ${s}\n  must not contain ${sub}`);
};
const same = (what, text) => {
  const a = on(text), b = off(text);
  if (a !== b) fail(`${what}: ${JSON.stringify(text)}\n  on  ${a}\n  off ${b}`);
};

has('a dictionary word', 'Hello there.', 'HH AH L OW');
has('its primary stress', 'Hello there.', "OW'");
// CMU: of AH1 V, was W AA1 Z, for F AO1 R, you Y UW1 -- all stressed alone.
has('function words take no stress, whatever CMU says', 'Of course it was for you.',
    "AH V K AO' R S IH T W AA Z F AO R Y UW");
has('the last of two primaries', 'Seventeen.', "IY'");
lacks('the last of two primaries', 'Seventeen.', "EH'");
has('a quoted word is found', "She said 'hello' twice.", 'HH AH L OW');
has('a possessive is its own entry', "John's hat.", 'JH');
has("a possessive plural is found", "The bosses' cars.", 'B AO');
has('a closing quote is tried off', "He said 'yes' and 'no'.", 'Y EH');
same('digits go to the number reader, not the dictionary', '3rd 21st 1984 $5');
// ABC and X are both CMU entries; glued to digits they must stay with the rules.
same('letters glued to digits stay with the rules', 'abc123 x86 mp3');
same('a word the dictionary lacks goes to the rules', 'Zyxquorbi vlemp.');
console.log('front end: the dictionary is used as decided');

if (failures) {
  console.error(`${failures} failure(s)`);
  process.exit(1);
}
console.log('dictionary: all checks pass');
