# Roadmap

The umbrella document: what is being built, in what order, and why that order.
Each phase has its own document; this one is the index, the ordering argument
and the status.

## The goal

Turn klattsch from a browser formant synthesizer into a native one — a C
engine, an NVDA add-on that needs no Python dependencies and no data files, and
a GUI sample generator that exposes every parameter the engine has, including
the ones upstream freezes as constants.

## What we are starting from

Established in phase 0 and recorded in [ARCHITECTURE.md](ARCHITECTURE.md);
repeated here because it sets the difficulty of everything below.

| | |
|---|---|
| **Engine** | ~1,000 lines of plain JavaScript, ES modules, no build step |
| **Dependencies** | zero at runtime; one optional (`cmu-pronouncing-dictionary`) |
| **Synthesis** | 3 parallel bandpass biquads, Rosenberg pulse derivative, xorshift noise |
| **Parameters** | 19 interpolated, plus ~30 constants frozen in the source |
| **Phonemes** | 40 ARPABET in the English bank; 3 banks |
| **Text front end** | none |
| **Tests** | **none** |
| **Build system** | none |
| **Targets** | browser (AudioWorklet), Node, CDN |

Two of those rows do more work than they look like they do. **There are no
tests**, so the goldens of phase 2 are the first tests this codebase will ever
have — there is no existing corpus to reuse or cross-check against, and the
corpus is therefore entirely our responsibility. And **there is no build
step**: what is in `src/` is exactly what npm publishes and what a CDN serves,
so the reference implementation cannot disagree with its own source.

## The order, and why

```
  Phase 0   Study                  -> ARCHITECTURE.md        done
  Phase 1   Compare                -> COMPARISON.md          done
            Screen-reader needs    -> SCREEN-READER.md       done
  Phase 2   Rewrite in C           -> REWRITE.md stages 0-6  not started
  Phase 3   Extend the engine      -> REWRITE.md stage 7
  Phase 4   GUI sample generator   -> GENERATOR.md
  Phase 5   NVDA add-on            -> NVDA-ADDON.md
```

Phases 2 and 3 are strictly sequential and everything else depends on them,
because of the first house rule: **the synthesis comes first.** A synthesizer
that sounds wrong is not fixable by packaging it better, and a bug found
through an NVDA add-on is a bug found in the worst possible place to debug it.
The C engine must render correct WAV files from a command line before anything
wraps it.

Phases 4 and 5 share the engine, the preset format and the front end, and
neither blocks the other — they can run in either order or in parallel. Phase 4
is listed first because the generator is how voices get designed and the add-on
wants designed voices to ship as presets. If the add-on is the more urgent
deliverable, swapping them costs nothing.

## How a stage is judged

Taken from the staged ports elsewhere in this tree, whose method is the part
that transfers even though their subject matter stays where it is.

- **Every stage has a named exit test** whose result is a number or a diff —
  not "the code is written" and not "it sounds right". A stage without a
  passing exit test is in progress, however finished the code looks.
- **One branch per stage.** `main` holds only stages whose exit test passes, so
  `main` is always a set of completed, verified stages rather than a work in
  progress.
- **A numbered chapter per stage** in `docs/`, recording what was read, what
  was measured and how each claim was established — not only the conclusion.
- **Measurements are part of the exit test**, so a performance regression is
  caught the same way a sample regression is.

Status key: ✅ done and verified · ◐ partly done, not verified · ○ not started

---

## Phase 0 — Study ✅

[ARCHITECTURE.md](ARCHITECTURE.md). The engine as it stands: signal path,
compiler, banks, and the complete parameter surface.

The finding that shaped everything after it: **most of what shapes klattsch's
voice is not a parameter.** Ten frozen compiler constants, a dozen hardcoded
shape ratios in `renderPhoneme`, and every mix constant in the DSP. The 19
interpolated parameters are the small half of the real surface.

## Phase 1 — Compare ✅

Two documents, because there are two different questions.

**[COMPARISON.md](COMPARISON.md)** — klattsch against the twelve synthesizers
surveyed in `..\speech synths overview.md`. Four findings:

1. **Three formants is the defining limit.** Every other formant synth in the
   folder has more. The bank format already anticipates `F4`+; the DSP does
   not. Formant count becomes a build dimension.
2. **The text front end is the blocking gap for NVDA, and it is already solved
   next door.** `votraxxion/src/ttv.c` does text → ARPABET in C with no
   allocation and no data files. klattsch speaks ARPABET. Its first stage lifts
   directly; its second is not needed. The largest schedule saving available to
   the project.
3. **Duration is the other missing model.** Every phoneme gets the same slot.
   Optional per-phoneme `durationMs`, bank schema v2, backward compatible.
4. **The compiler/synth split is the best thing klattsch has**, and is what
   makes the two halves independently verifiable during the rewrite.

**[SCREEN-READER.md](SCREEN-READER.md)** — what a screen reader needs that a
WAV renderer does not. Its §2 is a checklist (every index fires exactly once
including the ones carrying no audio; the idle flush without which say-all
stalls; generation-tagged cancellation; constant-pitch rate). Two of its
findings reach back into the engine and are scheduled in phase 3 rather than
left to phase 5: **latency** (§3) and **parameter audibility** (§5).

## Phase 2 — The C rewrite ○

[REWRITE.md](REWRITE.md), stages 0–6. C17, no allocation on the speech path,
seven translation units, CMake from stage 1, goldens captured from the
JavaScript before the first line of C.

The acceptance criterion is split, because a blanket "byte-identical" claim
would be dishonest against a JavaScript reference — V8 implements `sin`/`cos`
with its own fdlibm-derived code, so two correct implementations differ in the
last bits. The compiler and the integer DSP must match **exactly**; rendered
audio must show **zero differing samples after 16-bit quantization**.

Stage-by-stage exit tests, sizes and risks are the table in that document.
Stage 5 (`kl_compile.c`) is the only high-risk one: it is the largest
translation and the only stage where a difference is a logic difference rather
than a numeric one.

**Done when** the C CLI renders the whole corpus and every WAV is
byte-identical to the JavaScript CLI's, on MSVC, clang-cl and gcc.

### Prerequisites, settled before stage 0 begins

- [x] **Is `bundled.js` stale against its JSON?** It matters because stage 2
      generates the C banks from the same JSON, and a drift there would look
      like a porting bug for the rest of the port. **Checked 2026-09-23: the
      data is sound.** `tools/build-banks.js --check` reports the file out of
      date, but the regenerated file is byte-identical to the committed blob
      (both MD5 `f8c376a2ec1d1ec7082345e39129bbe8`). The guard compares
      on-disk bytes against freshly generated text, so a Windows checkout with
      `core.autocrlf=true` has CRLF where the generator writes LF, and it
      false-positives. The banks have not drifted.
- [x] **Fix the guard rather than living with it.** **Done 2026-09-23.** Both
      halves: `.gitattributes` pins `bundled.js` to LF so a checkout stops
      rewriting it, and `--check` normalizes line endings before comparing so
      it stays correct in a tree cloned before that pin existed. A
      line-endings-only difference now passes with a note naming the cause
      rather than failing. Verified on three cases — CRLF on disk passes with
      the note, LF on disk passes clean, and a deliberately altered formant
      value in `klatt1980-en.json` still fails with exit 1. That last case is
      the one worth keeping: a staleness guard that cannot fail is worse than
      no guard.
- [x] **Line endings decided for the goldens.** **Done 2026-09-23.**
      `.gitattributes` pins `goldens/**` to LF and every binary extension to
      `binary`, with the binary rules last so a captured WAV under `goldens/`
      is never converted. Decided before the first golden exists, which was
      the point.
- [x] **The guard is wired to something.** **Done 2026-09-23.** It ran nowhere
      — neither workflow invoked it. `.github/workflows/check.yml` now runs it
      on push and pull request. It joins the goldens in `ctest` at stage 8;
      until CMake exists there is no reason a Node check should wait for it.

## Phase 3 — Extend the engine ○

[REWRITE.md](REWRITE.md), stage 7. Each extension off by default, each landing
with the goldens re-run to prove the default path is unchanged. An extension
that alters the baseline is a bug, not a new voice.

- formant count 3–6 as a build dimension
- `FNP`/`FNZ` nasal pole and zero, through the `[FNZ=450]` syntax upstream
  already documents for an engine that did not exist yet
- jitter, shimmer, flutter, diplophonia — the cheapest naturalness wins
  available, all pure additions to the excitation stage
- per-phoneme intrinsic duration, bank schema v2
- an optional cascade path

### Three measurements that belong here, not to phases 4 and 5

All three come from [SCREEN-READER.md](SCREEN-READER.md), and all three are
cheap now and expensive later.

- [x] **Latency — measured 2026-09-23 on the JavaScript engine**, which is the
      baseline the C port inherits. **42–44× real time**, flat across a line, a
      45-second paragraph and a 20 ms chunk; compile time 1.4 ms for the
      paragraph. Throughput is ample and is *not* a reason to port. But
      rendering a paragraph whole costs **1.02 s before the first sample**,
      against **1.9 ms** chunked — a factor of ~500, available in any language.
      Full table in [SCREEN-READER.md](SCREEN-READER.md) §3.
- [ ] **Chunked rendering** in the C API, with cancel between chunks. Promoted
      by that measurement from "better" to **the design**: a whole-utterance
      loop in C would buy ~10× and leave a ~100 ms stall, which chunking in
      JavaScript already beats. The DSP needs no change — `kl_synth` renders
      into a caller's buffer of any length and keeps its state between calls.
- [ ] **Parameter audibility.** Every frozen constant rendered across its range
      and sorted into three lists: settings-ring material, generator-only, and
      inaudible or one-directional. This is a document, and it decides what
      gets a control in phases 4 and 5. Shipping a slider for a parameter
      nobody has listened to is how a settings panel fills with dead controls.

## Phase 4 — The GUI sample generator ○

[GENERATOR.md](GENERATOR.md). Native Win32, one x64 executable, linking the
same static engine the add-on does.

Every parameter in sections 1–6 of that document — the 19 live ones, the ~30
frozen constants promoted to real parameters, the phase 3 extensions, and
direct editing of the phoneme bank through an overlay that `extends` the base.
Which of them get a control is decided by the audibility measurement above.

Accessibility is a build requirement. A tool for designing screen-reader voices
that a screen-reader user cannot operate is not finished.

## Phase 5 — The NVDA add-on ○

[NVDA-ADDON.md](NVDA-ADDON.md). One Python shim, one x64 native library,
nothing else. 64-bit NVDA 2026.1 and later.

The work that is not the engine: the front end lifted from `ttv.c`, a stress
assignment pass, number and abbreviation normalization, and a sentence contour
pass that emits ordinary klattsch source and so is testable without audio.

[SCREEN-READER.md](SCREEN-READER.md) §2 is the acceptance checklist and §8 its
ordered steps. Index placement is a lookup rather than an estimate, because the
compiler's `phrases` array already carries `tStartMs`/`tEndMs` per token — a
place klattsch is better equipped than its neighbours.

---

## Cross-cutting rules

**64-bit only.** No x86 library, no 32-bit executable, no architecture switch,
no fallback path — house rule §4 of `..\CLAUDE.md`. Dropping 32-bit NVDA 2025
and earlier is the intended consequence.

**One version constant**, in the public C header, parsed by everything that
needs it: CMake's `project(VERSION)`, the add-on manifest, the GUI's
`VERSIONINFO`, the release script. Three places that "move together" is three
places that can drift.

**Built by more than one compiler, on more than one OS.** MSVC and clang-cl on
Windows, gcc on Linux, producing the same samples. A port verified on one
compiler is verified against that compiler's arithmetic, not against the
reference. Measured 2026-09-23: MSVC accepts C23 binary literals and digit
separators in `/std:c17` mode, so MSVC alone will not tell you your C17 is not
C17 — which is why all three run from stage 1, not at the end.

**C17, not C11 and not C23.** C11 was inherited without re-examination; C17 is
the same language with its defects fixed. C23 is out because MSVC 19.51 has no
`/std:c23` and its `clatest` lacks `constexpr`, `nullptr`, the `bool` keyword
and `#embed` — the features that would have been worth having. Measurement and
the forward path are in [REWRITE.md](REWRITE.md).

**The JavaScript engine stays**, frozen at 0.8.0, as the reference the goldens
come from. It is not a branch to track.

**Attribution travels with everything.** klattsch is Tony Gies's work; the MIT
notice and copyright go into every `csrc/` file, and each names the JavaScript
file it was translated from.

## Repository layout when this is finished

```
src/            the JavaScript engine — inherited, frozen, the reference
csrc/           the C engine
bin/            klattsch.mjs (JS CLI) and klattsch_cli.c (C CLI)
tools/          build-banks.js, build-banks-c.mjs, goldens.mjs
goldens/        captured from the JS, checked in, LF-pinned
gui-native/     the sample generator
nvda-addon/     manifest, shim, package.py
packaging/      release scripts and checksums
docs/           these documents, and a numbered chapter per port stage
CMakeLists.txt  the product build: library, CLI, tests
```

## Status

| Phase | State |
|---|---|
| 0 — Study | ✅ done |
| 1 — Compare | ✅ done |
| 1 — Screen-reader requirements | ✅ done |
| 2 — Rewrite (stages 0–6) | ◐ **stages 0–1 ✅**, stages 2–6 not started |
| 3 — Extend + measure | ○ not started |
| 4 — Generator | ○ not started |
| 5 — Add-on | ○ not started |

Stage-level checklists live in each phase's document, and the step logs there
are the record of what was actually measured.

## The next three things

1. **`kl_banks.c`** — stage 2, generated from the same JSON as `bundled.js`,
   with a CI check standing guard over the shared source of truth.
2. **`kl_synth.c`** — stage 3, driven by `goldens/schedules.json` so the
   sample loop is verified before the C compiler exists. This is the first
   stage where Tier 2 applies to whole rendered utterances rather than to a
   grid of one function.
3. **A gcc build**, on Linux or under MSYS2. Stage 1 passes on MSVC and
   clang-cl, but those share a CRT; the stage 6 exit test wants a genuinely
   different libm in the set.
