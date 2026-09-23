// Bundle src/engine/banks/*.json into src/engine/banks/bundled.js.
// Usage: `node tools/build-banks.js` (write) or `--check` (CI staleness).

import { readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const banksDir = join(here, '..', 'src', 'engine', 'banks');
const outFile = join(banksDir, 'bundled.js');

const SCHEMA_VERSION = 1;

function loadBanks() {
  const files = readdirSync(banksDir)
    .filter((f) => f.endsWith('.json'))
    .sort();
  const banks = {};
  for (const f of files) {
    const raw = readFileSync(join(banksDir, f), 'utf8');
    let bank;
    try {
      bank = JSON.parse(raw);
    } catch (err) {
      throw new Error(`bank file ${f} is not valid JSON: ${err.message}`);
    }
    if (bank.schemaVersion !== SCHEMA_VERSION) {
      throw new Error(
        `bank file ${f} has schemaVersion ${bank.schemaVersion}, ` +
          `generator supports ${SCHEMA_VERSION}`,
      );
    }
    if (!bank.name) throw new Error(`bank file ${f} is missing 'name'`);
    if (banks[bank.name]) {
      throw new Error(`duplicate bank name '${bank.name}' (in ${f})`);
    }
    banks[bank.name] = bank;
  }
  return banks;
}

function render(banks) {
  const header =
    '// Auto-generated from src/engine/banks/*.json by tools/build-banks.js.\n' +
    '// Do not edit by hand. Re-run the generator when banks change.\n' +
    '\n' +
    'export const bundled = ';
  return header + JSON.stringify(banks, null, 2) + ';\n';
}

const banks = loadBanks();
const rendered = render(banks);

// Staleness is a question about content, not about line endings. A Windows
// checkout with core.autocrlf=true has CRLF on disk where this generator
// writes LF, and a raw string comparison then reports every such checkout as
// stale even when the data is byte-identical to what is committed.
// .gitattributes pins this file to LF; normalizing here as well keeps the
// check correct in a tree that was cloned before that pin existed.
const normalizeEol = (s) => s.replace(/\r\n/g, '\n');

if (process.argv.includes('--check')) {
  let existing = '';
  try {
    existing = readFileSync(outFile, 'utf8');
  } catch {
    /* missing file: treated as stale */
  }
  if (normalizeEol(existing) !== normalizeEol(rendered)) {
    process.stderr.write(
      'bundled.js is out of date with src/engine/banks/*.json. ' +
        'Re-run `node tools/build-banks.js`.\n',
    );
    process.exit(1);
  }
  const banksCount = Object.keys(banks).length;
  if (existing !== rendered) {
    // Content matches; only the line endings differ. Not stale, but worth
    // saying, because the working tree does differ from what the generator
    // writes and a byte-for-byte tool other than this one would notice.
    process.stdout.write(
      `bundled.js up to date (${banksCount} banks); on-disk line endings are ` +
        'CRLF where the generator writes LF. Content is identical. ' +
        'See .gitattributes.\n',
    );
  } else {
    process.stdout.write(`bundled.js up to date (${banksCount} banks)\n`);
  }
} else {
  writeFileSync(outFile, rendered);
  process.stdout.write(
    `wrote ${outFile} (${Object.keys(banks).length} banks: ${Object.keys(banks).join(', ')})\n`,
  );
}
