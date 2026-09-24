// Generate csrc/kl_banks_data.c from src/engine/banks/*.json -- the same JSON
// that tools/build-banks.js turns into bundled.js, so the two languages can
// never read different data.
//
//   node tools/build-banks-c.mjs           write
//   node tools/build-banks-c.mjs --check   fail if the file is stale
//
// The banks are emitted **already resolved**: `extends` inheritance and `null`
// deletion are applied here, by the engine's own resolver, so the C carries
// flat tables and needs no resolution logic of its own. That is deliberate:
//
//   - it is the shape house rule 4 asks for ("a compiled-in table will do"),
//   - it needs no allocation and no JSON parser on the speech path,
//   - and resolution is not duplicated, so the two implementations cannot
//     disagree about what `extends` means -- there is only one implementation.
//
// The cost is that a bank supplied at *runtime* cannot use `extends` yet. That
// matters when the generator's overlay banks land (docs/GENERATOR.md section
// 5), not before, and it is recorded in docs/14-stage2-banks.md rather than
// discovered later.

import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { banks } from '../src/engine/index.js';

const here = dirname(fileURLToPath(import.meta.url));
const outFile = join(here, '..', 'csrc', 'kl_banks_data.c');

// The ten numbers the synthesizer actually reads, in a fixed order that the
// C struct, the dump tool and the verifier all share.
const NUMERIC = ['voicing', 'F1', 'F2', 'F3', 'BW1', 'BW2', 'BW3', 'A1', 'A2', 'A3'];

function cString(s) {
  if (s == null) return 'NULL';
  const escaped = String(s)
    .replace(/\\/g, '\\\\')
    .replace(/"/g, '\\"')
    .replace(/\n/g, '\\n')
    .replace(/\r/g, '\\r')
    .replace(/\t/g, '\\t')
    // Anything outside printable ASCII becomes a \u escape sequence in UTF-8
    // bytes, so the generated file is pure ASCII and no compiler has to be
    // told what encoding it is in. MSVC in particular will guess wrong.
    .replace(/[^\x20-\x7E]/g, (ch) => {
      const bytes = Buffer.from(ch, 'utf8');
      return [...bytes].map((b) => '\\x' + b.toString(16).padStart(2, '0')).join('');
    });
  return `"${escaped}"`;
}

// A double must survive the round trip exactly. %.17g is enough for any
// double, and JS's own shortest-round-trip formatting is shorter and equally
// exact -- but a bare integer like 310 would become an int literal, so a
// decimal point is forced.
function cDouble(x) {
  if (!Number.isFinite(x)) throw new Error(`non-finite bank value: ${x}`);
  const s = String(x);
  return /[.eE]/.test(s) ? s : `${s}.0`;
}

function render() {
  const names = banks.list().sort();
  const out = [];

  out.push('/* Generated from the JSON files in src/engine/banks by tools/build-banks-c.mjs.');
  out.push(' * Do not edit by hand. Re-run the generator when the banks change.');
  out.push(' *');
  out.push(' * Banks are emitted already resolved: `extends` and `null` deletion have');
  out.push(' * been applied, so these are flat tables. See the generator for why.');
  out.push(' *');
  out.push(' * SPDX-License-Identifier: MIT');
  out.push(' * Copyright (c) 2026 Tony Gies');
  out.push(' */');
  out.push('');
  out.push('#include "kl_banks.h"');
  out.push('');
  out.push('#include <stddef.h>');
  out.push('');

  for (const name of names) {
    const bank = banks.get(name);
    const ident = name.replace(/[^A-Za-z0-9]/g, '_');
    // Sorted by code so the lookup can binary-search, and so the generated
    // file is stable regardless of key order in the JSON.
    //
    // Every key is carried, including `_`. phonemes.js filters leading
    // underscores out of PHONEME_KEYS, but that is a *listing* convention for
    // a UI, not a data one: `_` is a real silence entry in the resolved bank.
    // The tokenizer cannot reach it (its phoneme pattern is [A-Z]+, so `_`
    // comes back as `unknown token`), so carrying it changes no behaviour --
    // but dropping it would make these tables stop mirroring the bank, and
    // then every comparison needs an exception somebody has to remember.
    const codes = Object.keys(bank.phonemes).sort();

    out.push(`/* ${name} -- ${bank.displayName} */`);
    if (bank.source) {
      // The bank's provenance is part of the data and is not dropped.
      out.push('/* source: ' + String(bank.source).replace(/\*\//g, '* /').replace(/\n/g, '\n * ') + ' */');
    }
    out.push(`static const kl_phoneme ph_${ident}[] = {`);
    for (const code of codes) {
      const p = bank.phonemes[code];
      const nums = NUMERIC.map((k) => cDouble(p[k] ?? 0)).join(', ');
      const glide = p.glideTo
        ? `1, { ${cDouble(p.glideTo.F1)}, ${cDouble(p.glideTo.F2)}, ${cDouble(p.glideTo.F3)} }`
        : '0, { 0.0, 0.0, 0.0 }';
      out.push(`    { ${cString(code)}, ${nums}, ${p.isStop ? 1 : 0}, ${glide},`);
      out.push(`      ${cString(p.ipa ?? null)}, ${cString(p.example ?? null)}, ${cString(p.source ?? null)} },`);
    }
    out.push('};');
    out.push('');
  }

  out.push('const kl_bank kl_banks[] = {');
  for (const name of names) {
    const bank = banks.get(name);
    const ident = name.replace(/[^A-Za-z0-9]/g, '_');
    const codes = Object.keys(bank.phonemes);
    out.push(`    { ${cString(bank.name)}, ${cString(bank.displayName)},`);
    out.push(`      ${cString(bank.language)}, ${cString(bank.license)}, ${cString(bank.source)},`);
    out.push(`      ${bank.schemaVersion}, ph_${ident}, ${codes.length} },`);
  }
  out.push('};');
  out.push('');
  out.push(`const size_t kl_bank_count = ${names.length};`);
  out.push(`const char *const kl_default_bank = ${cString(banks.defaultName)};`);
  out.push('');
  return out.join('\n');
}

const rendered = render();

if (process.argv.includes('--check')) {
  // Content, not line endings -- same reasoning as tools/build-banks.js.
  const norm = (s) => s.replace(/\r\n/g, '\n');
  const have = existsSync(outFile) ? norm(readFileSync(outFile, 'utf8')) : '';
  if (have !== norm(rendered)) {
    process.stderr.write(
      'csrc/kl_banks_data.c is out of date with src/engine/banks/*.json. '
      + 'Re-run `node tools/build-banks-c.mjs`.\n');
    process.exit(1);
  }
  process.stdout.write(`kl_banks_data.c up to date (${banks.list().length} banks)\n`);
} else {
  writeFileSync(outFile, rendered);
  const total = banks.list().reduce((n, b) => n + Object.keys(banks.get(b).phonemes).length, 0);
  process.stdout.write(
    `wrote ${outFile} (${banks.list().length} banks, ${total} resolved phonemes)\n`);
}
