# The C rewrite

## Why C and not C++

The survey in `..\speech synths overview.md` recommends modern C++ for a new
synthesis project. This is not a new project, and the argument that decided
`votraxxion` applies here unchanged:

1. **One toolchain.** Plain C17 builds under MSVC, MinGW, clang and gcc with no
   C++ ABI questions. The add-on needs an x64 Windows library, and a Linux
   build the day a Linux screen reader wants it. Everything here is 64-bit
   only, per house rule §4 of `..\CLAUDE.md`.
2. **No allocation on the speech path.** A synthesizer feeding an audio callback
   should never take the allocator lock. Fixed-capacity buffers with explicit
   bounds, sized above anything a screen reader hands over in one call.
3. **Size.** Template instantiation and exception tables for a program that
   never throws. The `votraxxion` C library is about 153 KB; that is the target
   shape.

And the shape of the thing agrees. `FormantSynth` is nine scalars, three biquad
structs and a loop. `compileSection` is a cursor over a token array. Neither
wanted a class in JavaScript and neither will want one in C.

The one place C++ would genuinely help is the GUI, and the GUI is a separate
binary that can be whatever it likes. See [GENERATOR.md](GENERATOR.md).

## Which C — measured, 2026-09-23

C17, not C11 and not C23. C11 was inherited from `votraxxion` without being
re-examined; C17 is the same language with its defect reports fixed and no new
features, so it is strictly better at no cost. Every compiler this project
targets has it.

C23 is the interesting question, and the answer is a toolchain fact rather
than a preference. **MSVC 19.51 (VS 18 Community, the toolset used here) has
no `/std:c23` at all** — `cl /?` advertises exactly `/std:<c11|c17|clatest>`.
`clatest` is a moving target, and what it actually implements was tested one
feature at a time:

| C23 feature | `/std:c17` | `/std:clatest` |
|---|---|---|
| `constexpr` objects | ✗ | ✗ |
| `nullptr` | ✗ | ✗ |
| `bool`/`true`/`false` as keywords | ✗ | ✗ |
| `#embed` | ✗ | ✗ |
| `typeof` | ✗ | ✓ |
| `[[nodiscard]]`, `[[maybe_unused]]` | ✗ | ✓ |
| binary literals `0b1010` | ✓ | ✓ |
| digit separators `1'000'000` | ✓ | ✓ |
| one-argument `static_assert` | ✓ | ✓ |

The three features that would actually earn their keep here — `constexpr` for
the compiled-in bank tables, `nullptr`, and `bool` without `<stdbool.h>` — are
exactly the three missing. `#embed` fails outright in every mode
(`fatal error C1021: invalid preprocessor command 'embed'`), and it would have
been the one genuine win: embedding tables and goldens without a generator.
As it is, `tools/build-banks-c.mjs` generates C source from the JSON anyway,
so `#embed` would not have been used even if it worked.

Against that, the cost of C23 is the exit test itself. Stage 6 requires MSVC,
clang-cl and gcc to produce identical samples; a dialect MSVC cannot compile
cannot be exit-tested, and the exit test is the spine of this plan. Choosing
C for toolchain reach and then picking the least portable C dialect would also
undo the argument that chose C in the first place.

**The forward path:** write C17 that is also valid C23. Avoid what C23 removed
— K&R function declarations, which this codebase would not contain anyway —
and revisit when MSVC ships a real `/std:c23`. Nothing in the plan depends on
a C23 feature, so that revisit is free whenever it happens.

### One trap the same test exposed

MSVC accepted **binary literals and digit separators under `/std:c17`**, where
they are not C17 features at all. MSVC's C mode is lenient, so C23-isms can
enter the codebase and compile cleanly on Windows while `gcc -std=c17
-pedantic` rejects them. That is a concrete reason for the rule below that all
three compilers run from stage 1 rather than at the end: MSVC alone will not
tell you your C17 is not C17.

## The acceptance criterion

`votraxxion`'s rule was **byte-identical output or it is a bug**, and it could
hold that rule because the C++ it replaced and the C that replaced it both ran
on the same machine with the same libm.

**That rule cannot honestly be claimed here, and pretending otherwise would
hide real bugs behind a tolerance.** klattsch's DSP calls `Math.sin`, `Math.cos`
and `Math.pow` every sample, and V8 implements those with its own
fdlibm-derived code rather than the platform libm. Two correct implementations
will differ in the last bits. So the criterion is split:

**Tier 1 — exact, no tolerance.** Everything that is integer or rational
arithmetic must match bit for bit:

- the tokenizer's classification of every token in the corpus,
- the compiler's schedule: event count, `atMs`, `transitionMs`, and every
  target field, compared as exact IEEE-754 doubles,
- the xorshift LFSR state sequence, all 2^32 - 1 states if you like, but at
  minimum the first million from the seed `0xACE1ACE1`,
- sample counts, WAV header bytes, and the `ICMT` round-trip.

A single differing schedule field is a bug, not a rounding difference. The
compiler does no transcendental arithmetic except `noteToHz`, which is the one
declared exception and is compared to within 1e-9 Hz.

**Tier 2 — bounded, and the bound is tight.** Rendered audio, compared against
the JS reference:

- peak absolute sample difference `<= 1e-9` on a 30-second corpus, in float64
  before quantization,
- after 16-bit quantization, **zero differing samples**. A 1e-9 float
  difference cannot move a 16-bit sample unless it sits exactly on a rounding
  boundary; if any sample differs, that is investigated, not waived.

If Tier 2 cannot be met, the cause is a logic difference, not libm. Every time
this has been tested on a formant synth the transcendental error has stayed
five orders of magnitude below the 16-bit LSB.

**Goldens are captured from the JS before the first line of C is written**, by
`tools/goldens.mjs`, and checked in. The corpus has to be adversarial about the
things a rewrite actually breaks:

- every phoneme in all three banks, in isolation, and in a vowel frame,
- every directive, in absolute, relative, and bare-reset form,
- note-name pitch across the full `A-1` to `G#9` range,
- both pitch-delta forms, sticky and transient, at boundaries,
- syllable groups including the malformed ones: nested `(`, unmatched `)`,
  unclosed `(`, each of which has a defined behaviour in the JS that the C must
  reproduce,
- bank switching mid-utterance and `[bank]` reset,
- `[voice=N]` with 1, 2 and 5 sections, including an empty section,
- uppercase extras set, overridden and cleared,
- unknown phonemes and unknown tokens — the warning strings are part of the
  contract,
- comments in every position, including a block comment splitting a token,
- homoglyph and zero-width normalization,
- `rate` low enough that the `min()` caps in `renderPhoneme` bind, and high
  enough that they do not,
- the empty string.

## The shape

`csrc/`, C17, one translation unit per concern, each with a header:

| File | From | Notes |
|---|---|---|
| `kl_dsp.{h,c}` | `src/engine/dsp.js` | Biquad, glottal pulse, xorshift, soft clip. Mechanical, but the LFSR needs care — see below. |
| `kl_synth.{h,c}` | `src/engine/synth-core.js` | The sample loop. `PARAMS` becomes a fixed struct, not a map. |
| `kl_banks.{h,c}` | `src/engine/banks/*` | Banks compiled in as static tables, generated from the JSON by `tools/build-banks-c.mjs` — the same JSON stays the source of truth, and the JS and C banks can never drift. |
| `kl_token.{h,c}` | `tokenize()` | Normalization tables, comment stripping, classification. |
| `kl_compile.{h,c}` | `compileSection()`, `compile()` | The cursor and the four phoneme shapes. Schedule into a caller-provided arena. |
| `kl_wav.{h,c}` | `src/engine/wav.js` | RIFF/WAVE with the LIST INFO chunk. |
| `klattsch.{h,c}` | `src/engine/index.js` | The public C API and the one-call convenience path. |

`bin/` gains `klattsch_cli.c`, the C equivalent of `bin/klattsch.mjs`, which is
what proves the engine before any add-on or GUI exists.

The JavaScript stays. This fork does not delete upstream's engine — it is the
reference implementation that the goldens come from and that regressions are
measured against, permanently.

### Attribution in the ported code

A translation of someone's algorithm is still their algorithm. Every file in
`csrc/` carries the MIT notice and `Copyright (c) 2026 Tony Gies` alongside any
notice for work added here, and names the JavaScript file it was translated
from — the `From` column of the table above is that record, and it belongs in
the file header too, not only in this document. The same applies to anything
lifted from a neighbouring synthesizer: `ttv.c` arrives with its own history
intact and a header saying where it came from and what was removed.

## The numeric hazards, written down before they bite

Each of these is a place where a mechanical translation is wrong.

**The LFSR is signed.** `xorshift` uses `x >>> 17`, a *logical* shift on the
unsigned reinterpretation, while `x << 13` and `x << 5` are wrapping signed
shifts, and `noiseSample = lfsr / 2147483648` divides the **signed** int32. In
C: do the shifts in `uint32_t`, then reinterpret to `int32_t` for the divide.
Getting this wrong gives noise in [0, 2) instead of [-1, 1) and a DC offset
through every fricative.

**Everything is double, output is float.** JavaScript arithmetic is float64
throughout; only the store into `Float32Array` rounds. The C loop must compute
in `double` and round once, on the store to `float`. A `float` accumulator in
the biquads will drift audibly over a long utterance.

**`Math.floor` on negatives.** Both phase wraps use `x -= floor(x)`. C's
`floor` agrees, but an `(int)` cast does not — it truncates toward zero. Use
`floor`.

**Coefficient caching is observable.** `BandpassBiquad.setFreq` returns early
when `(f, bw)` are unchanged *before* clamping, so the cache key is the raw
values and the clamp is applied after. During a transition the values change
every sample and the cache never hits; when a parameter is held, it always
hits. Reproduce the ordering exactly or the clamped-range behaviour differs.

**Property order in the emitted target.** The JS builds each target as the
phoneme fields, then `extras`, then voice state, so a bank that defined
`aspiration` would be overwritten by the running `aspiration`, and an
uppercase extra is overwritten by nothing. With a fixed struct this ordering
has to be reimposed deliberately as an assignment order.

**Stray fields ride along.** `scaled()` spreads the whole phoneme object, so
`isStop` and `glideTo` end up inside schedule targets. `FormantSynth` ignores
them because it iterates `PARAMS`. The C struct simply will not have them —
which is correct, and is a difference the golden comparison must be told to
expect, in exactly one place, rather than silently tolerating extra keys.

**`0.1 + 0.2`.** Time accumulates as repeated `timeMs += slotMs` in float64.
Reproduce the accumulation order exactly; do not "improve" it by multiplying an
index by a slot width.

## The order

Bottom-up, so each stage is verifiable before the next depends on it.

**Every stage has an exit test, and the exit test is what "done" means.** Not
"the code is written" and not "it sounds right" — a named, runnable check whose
result is a number or a diff. A stage without a passing exit test is in
progress, however finished the code looks. This discipline is taken from the
staged ports elsewhere in this tree, which is where it earned its keep: it is
the thing that makes a long port recoverable when a later stage exposes a
mistake in an earlier one.

Status key: ✅ done and verified · ◐ partly done, not verified · ○ not started

| # | Stage | Exit test | Size | Risk | Status |
|---|---|---|---|---|---|
| 0 | **Baseline.** `tools/goldens.mjs` against the JS engine, the corpus above, goldens and their generator checked in | The generator re-run twice produces identical goldens; the corpus covers every item in the list above, and a deliberately broken JS engine is caught by it | medium | low | ○ |
| 1 | **`kl_dsp.c`** — biquad, pulse, LFSR, soft clip | 1,000,000 LFSR states exact; biquad coefficients across the (f, bw) grid and the pulse across phase × effort within 1e-12 of the JS | small | low | ○ |
| 2 | **`kl_banks.c`** + `tools/build-banks-c.mjs` | All three banks, field by field, identical to the resolved JS banks, with `extends` and `null` deletion exercised | small | low | ○ |
| 3 | **`kl_synth.c`** — the sample loop, driven by a golden schedule from JSON so the compiler is not yet involved | Tier 2 on every schedule in the corpus: peak difference ≤ 1e-9, zero differing samples after 16-bit quantization | medium | medium | ○ |
| 4 | **`kl_token.c`** | Every corpus token classified identically, exact, including the malformed ones | small | low | ○ |
| 5 | **`kl_compile.c`** — the four shapes, directives, syllables, voices, banks, extras, warnings | Tier 1 on the whole corpus: event count, `atMs`, `transitionMs` and every target field exact as IEEE-754 doubles; warning strings identical | medium | **high** | ○ |
| 6 | **`kl_wav.c`** + `bin/klattsch_cli.c` | The CLI renders the whole corpus and every WAV is byte-identical to the JS CLI's, on MSVC, clang-cl and gcc | small | low | ○ |
| 7 | **Extensions**, each off by default | The stage-6 exit test still passes unchanged with every extension compiled in and defaulted off | medium | medium | ○ |
| 8 | **Regression** — goldens in `ctest`, run in CI | A deliberately introduced one-sample error fails the build | small | low | ○ |

Stages 0–6 are the rewrite. Stage 7 is the part that makes it worth having
done, and nothing in stage 7 begins until stage 6 is green.

Stage 5 is the only high-risk one: it is the largest translation, it is the
only stage where a difference is a *logic* difference rather than a numeric
one, and it is where the six hazards above mostly live. It sits after stage 3
so that the sample loop — the part that is hard to debug by reading — is
already known good when the compiler is under test.

### Working rules

- **One branch per stage; `main` holds only verified stages.** Each stage is
  developed on its own branch (`stage0-goldens`, `stage1-dsp`, …) and merges to
  `main` only when its exit test passes. `main` is then always a set of
  completed, verified stages rather than a work in progress.
- **Build with CMake from stage 1**, not at the end. The multi-compiler exit
  test of stage 6 only works if it has been possible to run it all along, and a
  port verified on one compiler is verified against that compiler's arithmetic
  rather than against the reference.
- **A chapter per stage**, numbered, in `docs/`: what was read, what was
  measured, and how each claim was established — not just the conclusion. The
  step log below is the index into those chapters.
- **Measurements are part of the exit test.** Compile time and render speed
  ([SCREEN-READER.md](SCREEN-READER.md) §3) are captured by the golden harness
  from stage 3 onward, so a performance regression is caught the same way a
  sample regression is.

## Step log

Kept short on purpose: what changed, and what proved it.

*(empty — step 0 has not started)*
