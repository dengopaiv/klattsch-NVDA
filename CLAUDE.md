# klattsch-NVDA

This is a fork of [tgies/klattsch](https://github.com/tgies/klattsch), a
parallel-formant speech synthesizer written in JavaScript for the browser.
Upstream is a web toy and an npm package. This fork has a different goal.

The goal here is to turn that engine into a **native speech synthesizer for
screen-reader use**: the DSP and the compiler rewritten in C, packaged as an
NVDA add-on that needs no Python dependencies and no data files, and as a GUI
sample generator that exposes every parameter the engine has -- including the
ones upstream froze as constants.

Four pieces of work, in order, each finishable before the next begins:

1. **Study the engine.** Written down in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
2. **Compare it to the other synthesizers** in `C:\git\speech synthesis\`, to
   know what it has, what it lacks, and what is worth borrowing.
   [docs/COMPARISON.md](docs/COMPARISON.md).
3. **Rewrite the engine in C.** [docs/REWRITE.md](docs/REWRITE.md).
4. **Ship it**: an NVDA add-on ([docs/NVDA-ADDON.md](docs/NVDA-ADDON.md)) and a
   GUI sample generator ([docs/GENERATOR.md](docs/GENERATOR.md)).

[docs/ROADMAP.md](docs/ROADMAP.md) is the umbrella: phases, ordering, status.

## House rules

- **The synthesis comes first.** Get the C engine matching the JS engine and
  rendering correct WAV files from the command line before anything is wrapped
  in an add-on or a GUI. A synthesizer that sounds wrong is not fixable by
  packaging it better.
- **A rewrite that "sounds the same" is a rewrite nobody can trust.** There is
  a numeric acceptance criterion in [docs/REWRITE.md](docs/REWRITE.md), and
  goldens captured from the JS before the first line of C is written.
- **No runtime dependencies in the shipped artifacts.** The pattern to follow is
  `votraxxion`: one native library per architecture, one Python shim, nothing
  else. Not 40 MB of vendored wheels.
- **Klatt is the reference, not the constraint.** klattsch is a deliberately
  primitive three-formant parallel synth. Where the C port grows past that
  (cascade path, nasal pole/zero, more formants), it is an additive option with
  a documented default that reproduces the original, never a silent change.
- **Record what turned out to be wrong.** When an assumption in these documents
  is disproved, correct it in place and say what the measurement was. A plan
  with its mistakes edited out teaches nothing.

## This is a fork, and it stays one

klattsch is Tony Gies's work. This fork alters it fundamentally, and that
changes nothing about whose work it is.

- **Attribution is paramount.** The fork relationship on GitHub stays. The
  `LICENSE` stays as it is — MIT, Copyright (c) 2026 Tony Gies — and upstream's
  copyright notice travels with every artifact this repository produces,
  including the C rewrite, the NVDA add-on and the GUI. A port of someone's
  algorithm is still their algorithm.
- **Say what came from where.** The C engine is a translation of upstream's
  JavaScript, and the documents here name the file each translation unit comes
  from. Anything borrowed from a neighbouring synthesizer is credited the same
  way. Where a phoneme bank is somebody's published data, its `source` field is
  part of the data and is not dropped.
- **No upstreaming.** Nothing here is written with a pull request to
  `tgies/klattsch` in mind. If the author ever wants any of it he can pull it
  from here; that is his decision and not a goal that shapes these documents.
- **No tracking upstream either.** The JavaScript engine is kept frozen as the
  reference implementation the goldens come from, not as a branch to merge
  from. A specific upstream change can be taken for a specific reason.
- **The baseline is fixed and recorded.** The inherited history ends at
  `43189e6` (`chore(release): 0.8.0`, 2026-07-26); the DSP and compiler were
  last touched 2026-07-06. Forked 2026-09-11. The goldens in
  [docs/REWRITE.md](docs/REWRITE.md) are captured from that baseline, so what
  the C engine is verified against stays pinned regardless of what upstream
  does later.
