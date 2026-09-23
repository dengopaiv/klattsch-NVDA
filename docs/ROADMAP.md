# Roadmap

The umbrella document. What is being built, in what order, and why that order.

## The goal

Turn klattsch from a browser formant synthesizer into a native one: a C engine,
an NVDA add-on that needs no Python dependencies and no data files, and a GUI
sample generator that exposes every parameter the engine has — including the
ones upstream freezes as constants.

## The order, and why

```
  Phase 0   Study             -> ARCHITECTURE.md          [done]
  Phase 1   Compare           -> COMPARISON.md            [done]
                                 SCREEN-READER.md         [done]
  Phase 2   Rewrite in C      -> REWRITE.md stages 0-6    [not started]
  Phase 3   Extend the engine -> REWRITE.md stage 7
  Phase 4   GUI generator     -> GENERATOR.md
  Phase 5   NVDA add-on       -> NVDA-ADDON.md
```

Every stage in phase 2 has a **named exit test** and `main` holds only stages
whose exit test passes ([REWRITE.md](REWRITE.md), "The order"). That discipline,
and the habit of writing a numbered chapter per stage recording how each claim
was established rather than only the conclusion, is taken from the staged ports
elsewhere in this tree. It is the part of their method that transfers.

Phases 2 and 3 are strictly sequential and everything else depends on them. The
reason is the rule from `CLAUDE.md`: **the synthesis comes first.** A
synthesizer that sounds wrong is not fixable by packaging it better, and a bug
found through an NVDA add-on is a bug found in the worst possible place to
debug it. The C engine must render correct WAV files from a command line before
anything wraps it.

Phases 4 and 5 can run in either order or in parallel — they share the engine,
the preset format and the front end, and neither blocks the other. Phase 4 is
listed first because the generator is how the voice gets designed, and the
add-on wants designed voices to ship as presets. If the add-on is the more
urgent deliverable, swapping them costs nothing.

## Phase 0 — Study *(done)*

[ARCHITECTURE.md](ARCHITECTURE.md). The engine as it stands: signal path,
compiler, banks, and the complete parameter surface.

The finding that shaped everything after it: **most of what shapes klattsch's
voice is not a parameter.** Ten compiler constants, a dozen shape ratios, and
every mix constant in the DSP are frozen in the source. The engine's 19
interpolated parameters are the small half of its real parameter surface.

## Phase 1 — Compare *(done)*

Two documents: [COMPARISON.md](COMPARISON.md) places the engine against the
twelve synthesizers surveyed in `..\speech synths overview.md`, and
[SCREEN-READER.md](SCREEN-READER.md) is the separate question of what a screen
reader needs that a WAV renderer does not.

### What the comparison found

Four things:

1. **Three formants is the defining limit.** Every other formant synth in the
   folder has more. The bank format already anticipates `F4`+; the DSP does
   not. Formant count becomes a build dimension in the C engine.
2. **The text front end is the blocking gap for NVDA, and it is already
   solved next door.** `votraxxion/src/ttv.c` does text → ARPABET in C with no
   allocation and no data files. klattsch speaks ARPABET. Stage 1 lifts
   directly; stage 2 is not needed. The largest schedule saving available.
3. **Duration is the other missing model.** Every phoneme gets the same slot.
   An optional per-phoneme `durationMs` in bank schema v2, backward compatible.
4. **The compiler/synth split is the best thing klattsch has** and is what makes
   the two halves independently verifiable during the rewrite. Do not lose it.

### What screen-reader suitability adds

[SCREEN-READER.md](SCREEN-READER.md) collects the requirements that are cheap
to design in and expensive to retrofit — every index firing exactly once
including the ones carrying no audio, the idle flush without which say-all
stalls, generation-tagged cancellation, and constant-pitch rate. They are
written as a checklist because that is how they will be used: once when the
driver is written, and again every time it misbehaves.

Two findings there change work earlier in the plan:

- **Latency is a design constraint on the C engine, not an add-on concern.**
  Time to first audio currently scales with utterance length, because the whole
  utterance is rendered before anything plays. Chunked rendering with cancel
  between chunks fixes it and costs the DSP nothing — but the two numbers that
  decide how urgent it is, compile time and render speed as a multiple of real
  time, have never been measured. They join the golden harness at stage 3.
- **Measure a parameter before giving it a control.** The roughly thirty frozen
  constants get rendered across their ranges and sorted into three lists:
  settings-ring material, generator-only, and inaudible or one-directional.
  Shipping a slider for a parameter nobody has listened to is how a settings
  panel fills with controls that do nothing.

## Phase 2 — The C rewrite

[REWRITE.md](REWRITE.md), steps 0–6. C11, no allocation on the speech path,
seven translation units, goldens captured from the JavaScript before the first
line of C.

The acceptance criterion is split, because a blanket "byte-identical" claim
would be dishonest here: the compiler and the integer DSP must match **exactly**,
and rendered audio must match to **zero differing samples after 16-bit
quantization**. Reasoning and the full corpus are in that document, along with
the six numeric hazards that make a mechanical translation wrong.

The JavaScript engine stays. It is the reference implementation the goldens come
from, permanently.

Done when `bin/klattsch_cli.c` renders the whole corpus and every WAV is
byte-identical to the JavaScript CLI's.

## Phase 3 — Extend the engine

[REWRITE.md](REWRITE.md), step 7. Each extension off by default, each landing
with the goldens re-run to prove the default path is unchanged. An extension
that alters the baseline is a bug, not a new voice.

- formant count 3–6 as a build dimension
- `FNP`/`FNZ` nasal pole and zero, through the `[FNZ=450]` syntax upstream
  already documents for an engine that did not exist yet
- jitter, shimmer, flutter, diplophonia — the cheapest naturalness wins
  available, all pure additions to the excitation stage
- per-phoneme intrinsic duration, bank schema v2
- an optional cascade path

## Phase 4 — The GUI sample generator

[GENERATOR.md](GENERATOR.md). Native Win32, one x64 executable, linking the
same static engine the add-on does.

Every parameter in sections 1–6 of that document, which is what "all the
possible parameters exposed" resolves to: the 19 live ones, roughly thirty
frozen constants promoted to real parameters, the extensions from phase 3, and
direct editing of the phoneme bank through an overlay that `extends` the base.

Accessibility is a build requirement. A tool for designing screen-reader voices
that a screen-reader user cannot operate is not finished.

## Phase 5 — The NVDA add-on

[NVDA-ADDON.md](NVDA-ADDON.md). One Python shim, one x64 native library,
nothing else. 64-bit NVDA 2026.1 and later.

The work that is not the engine: the front end lifted from `ttv.c`, a stress
assignment pass, number and abbreviation normalization, and a sentence contour
pass that emits ordinary klattsch source and so is testable without audio.

The shim follows `votraxxion`'s: one thread touching the engine, cancellation as
an epoch counter, control items exempt from it. Index callbacks are a lookup
rather than an estimate, because the compiler's `phrases` array already carries
`tStartMs`/`tEndMs` per token — a place klattsch is better equipped than its
neighbours.

Rate is constant-pitch. A screen-reader user turning the speed up does not
expect the voice to rise.

[SCREEN-READER.md](SCREEN-READER.md) §2 is the acceptance checklist for this
phase, and §8 its ordered steps.

## Repository layout when this is finished

```
src/            the JavaScript engine — upstream's, kept as the reference
csrc/           the C engine
bin/            klattsch.mjs (JS CLI) and klattsch_cli.c (C CLI)
tools/          build-banks.js, build-banks-c.mjs, goldens.mjs
goldens/        captured from the JS, checked in
gui-native/     the sample generator
nvda-addon/     manifest, shim, package.py
packaging/      release scripts and checksums
docs/           these documents, and a numbered chapter per port stage
```

## Status

| Phase | State |
|---|---|
| 0 — Study | done |
| 1 — Compare | done |
| 1 — Screen-reader requirements | done |
| 2 — Rewrite | not started |
| 3 — Extend | not started |
| 4 — Generator | not started |
| 5 — Add-on | not started |

Step-level checklists live in each phase's own document, and the step logs
there are the record of what was actually measured.
