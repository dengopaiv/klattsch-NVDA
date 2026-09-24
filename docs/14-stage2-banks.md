# 14. Stage 2 — `kl_banks.c`

Stage 2 of the [C rewrite](REWRITE.md): the phoneme banks, generated from the
same JSON that produces `bundled.js`. Branch `stage2-banks`, generator
`tools/build-banks-c.mjs`, code in `csrc/kl_banks.{h,c}` and the generated
`csrc/kl_banks_data.c`, verified by `tools/verify-stage2.mjs`.

Bank data is the quietest way a synthesizer can be wrong. One formant in one
phoneme of one bank changes a single vowel and nothing else, and nothing
crashes, and no test that is not looking at that number will ever notice.

## 14.1 The decision: resolved at generation, not at run time

The JS registry resolves `extends` inheritance and `null` deletion at run time,
with a cache. The C does not: **the generator emits banks already resolved**,
so `csrc/kl_banks_data.c` is flat tables and `kl_banks.c` is lookup only.

Three reasons:

- It is the shape house rule §4 asks for — "a compiled-in table will do".
- No allocation and no JSON parser on the speech path.
- **Resolution is not duplicated**, so the two implementations cannot disagree
  about what `extends` means. There is only one implementation of it, in
  JavaScript, and the C consumes its output.

The generator imports the engine's own `banks` registry and emits what it
returns. The C bank data is therefore, by construction, the JS's resolution.

**The cost, recorded now rather than discovered later:** a bank supplied at
*run time* cannot use `extends` yet, because nothing in C resolves it. That
becomes real when the generator's overlay banks land
([GENERATOR.md](GENERATOR.md) §5), which is phase 4. Either the C grows a
resolver then, or overlays are flattened before they reach it. Not deciding
now, but not forgetting either.

## 14.2 What the tables carry

Ten numbers per phoneme — `voicing`, `F1`–`F3`, `BW1`–`BW3`, `A1`–`A3` — plus
`is_stop`, an optional `glideTo` triple, and three strings: `ipa`, `example`
and `source`.

The strings are not read by the synthesizer. They are carried anyway, because
house rule: *"Where a phoneme bank is somebody's published data, its `source`
field is part of the data and is not dropped."* The Mokhtari & Tanaka bank's
source is a citation with an archive URL; the Hecko bank credits a named
contributor. Dropping those to save a pointer is not a trade this project
makes, and the verifier compares them as strictly as it compares a formant.

The generated file is **pure ASCII** — every non-ASCII byte in an IPA symbol or
a citation becomes a `\xNN` escape — so no compiler has to be told what
encoding it is in. MSVC in particular will guess, and guess wrong.

## 14.3 A bug found before the verifier existed

The first generated file had 39 phonemes for the English bank where the
resolved bank has 40, and 45 where the Japanese banks have 46.

The cause: `src/engine/phonemes.js` builds `PHONEME_KEYS` by filtering out keys
that start with `_`, and I copied that filter into the generator. **It is a
listing convention, not a data convention.** `_` is a real entry in
`klatt1980-en` — a silence with `voicing: 0` and all amplitudes at zero — and
`PHONEME_KEYS` exists to populate a UI, not to describe the bank.

Dropping it changes no behaviour: the tokenizer's phoneme pattern is `[A-Z]+`,
so `_` never reaches a lookup, and `AA _ AA` compiles with
`unknown token: _`. But it would have made the C tables stop mirroring the
bank, and then every comparison needs an exception that somebody has to
remember. Every key is carried instead. `_` sorts after `Z` in both JS string
order and `strcmp`, so the binary search is unaffected.

## 14.4 The exit test

> All three banks, field by field, identical to the resolved JS banks, with
> `extends` and `null` deletion exercised.

✅ Passes on all four toolchains. **2115 fields across 132 phonemes**, compared
individually rather than by digest, so a failure names the bank, the phoneme
and the field.

*Measured by hand on the development machine with `tools/build-matrix.ps1`,
which builds and verifies all four. CI runs the Ubuntu gcc leg only, so this
is a result with a date on it rather than a property enforced on every push —
[ROADMAP.md](ROADMAP.md), cross-cutting rules.*

```
Bank data -- Tier 1, exact
  bank count                             ok   3
  default bank                           ok   klatt1980-en
  bank[0] ja-hecko-2026 metadata         ok   6 fields
  bank[0] ja-hecko-2026 phonemes         ok   46 entries
  bank[1] ja-mokhtari-2000 metadata      ok   6 fields
  bank[1] ja-mokhtari-2000 phonemes      ok   46 entries
  bank[2] klatt1980-en metadata          ok   6 fields
  bank[2] klatt1980-en phonemes          ok   40 entries
  stream fully consumed                  ok   17736 bytes
  fields compared                        ok   2115 across 132 phonemes
```

**Everything here is Tier 1.** Bank data is numbers copied from a table with no
arithmetic between the JSON and the C, so there is no tolerance to hide behind
and no libm involved. The four toolchains produce **byte-identical** dumps —
`135cc92e77d96939` for the banks and `bdde3ede4931b631` for the probe — on
MSVC, clang-cl, WinLibs gcc and WSL's glibc gcc alike. That is the expected
contrast with stage 1, where `glottalPulse` differed under glibc: no
transcendental, no divergence.

### Lookup is tested, not just data

A table that is byte-perfect behind a binary search that cannot find its last
entry is not a working bank. The `probe` section looks up **every code in every
bank** and checks the returned pointer is the table row itself, then checks
that absent codes miss — empty string, lowercase, embedded space, and codes
either side of the sorted range — and that NULL arguments return NULL rather
than crashing.

### `null` deletion has no coverage in the shipped data

**No shipped bank uses `null` deletion.** Both Japanese banks extend
`klatt1980-en` and only add entries. So the resolver path that handles deletion
is not exercised by the data at all, and since the C inherits its correctness
from that resolver, neither is the C.

The verifier therefore registers a fixture bank that deletes an inherited
entry, overrides another, and adds a third, and checks all four outcomes plus
that the parent is left untouched. It is a fixture rather than a shipped bank
because inventing bank data to satisfy a test would be worse than testing the
mechanism directly.

### The comparison can fail

`tools/stage2-mutations.sh` corrupts the generated table one field at a time
and requires the verifier to fail on each. **6 of 6 caught**: a formant, a
bandwidth, a voicing flag, an `is_stop` change, a dropped `source` string, and
a broken sort order.

The dropped-source mutation is there deliberately. A comparison that skips the
provenance strings would pass while the attribution quietly disappeared, which
is a licensing problem rather than a cosmetic one.

## 14.5 Two guards over one source of truth

`src/engine/banks/*.json` now feeds two generated files:

| Generated | By | Guarded by |
|---|---|---|
| `src/engine/banks/bundled.js` | `tools/build-banks.js` | `banks-current` (ctest, CI) |
| `csrc/kl_banks_data.c` | `tools/build-banks-c.mjs` | `banks-c-current` (ctest, CI) |

Both guards normalize line endings before comparing, for the reason recorded in
[ROADMAP.md](ROADMAP.md): the first one false-positived on every Windows
checkout until it did.

## 14.6 Step log

**Generator written**, emitting resolved banks from the engine's own registry
so resolution exists in exactly one place.

**Bug found and fixed before the verifier ran** — the `_` filter, §14.3.

**`kl_banks.{h,c}`**: flat tables, `strcmp` bank lookup over three entries,
binary search for phonemes. Clean at `/W4` and `-Wall -Wextra -Wpedantic
-Wshadow -Wconversion`.

**Exit test passes** on MSVC, clang-cl, WinLibs gcc and WSL glibc gcc, all four
byte-identical. 2115 fields, 132 phonemes.

**Mutation test 6 of 6 caught**, including a dropped attribution string.

**`ctest` is 5 tests**, 8.9 s: two stage verifiers and three currency guards.
