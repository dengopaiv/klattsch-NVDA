# klattsch against the other synthesizers

`C:\git\speech synthesis\` holds thirteen speech synthesizers studied for this
project. Twelve of them are already surveyed in
`..\speech synths overview.md`. klattsch is the thirteenth and was not in that
survey; this document places it, and draws out the specific things worth taking
from the neighbours.

## Where klattsch sits

| | klattsch |
|---|---|
| **Synthesis type** | Parallel formant, 3 resonators. No cascade path. |
| **Voiced source** | Rosenberg pulse derivative, one `effort` knob for shape |
| **Unvoiced source** | 32-bit xorshift LFSR |
| **Filtering** | 3 RBJ constant-skirt bandpass biquads in parallel |
| **Phonemes** | 40 ARPABET in the English bank, 3 banks total |
| **Text front end** | None (optional CMU dictionary lookup, no rules) |
| **Prosody** | Per-phoneme pitch deltas, stress lift, punctuation pauses |
| **Sample rate** | Anything; 48 kHz by default |
| **Language** | JavaScript, zero dependencies |

Against the survey's families, klattsch is in the **"formant, other"** group
with Wintalker, Votraxxion, BeSTspeak and TruVoice: formant synthesis that is
not Klatt's cascade/parallel architecture, even though its phoneme table is
taken verbatim from Klatt 1980. The engine is simpler than every Klatt-family
member here (DECtalk, Manatu, RSynth, SynthFix) and simpler than Wintalker.
Only SAM and STSpeech are structurally simpler, and both of those are tied to
8-bit sound-chip tricks that klattsch does not use.

The honest summary: **klattsch has the cleanest architecture and the weakest
model.** The compiler/synth split is better factored than anything else in the
folder, and there is less synthesis in it than in anything except SAM.

## Three formants is the defining limit

Every other formant synth here has more:

| Synth | Formants |
|---|---|
| DECtalk | 6 cascade + parallel |
| Manatu | F1–F3 at 4th order, F4–F6 at 2nd order, cascade + parallel |
| Wintalker | cascade F1–F4 + nasal, parallel F2p–F6p |
| Votraxxion | F1, F2v, F3, F4, FX |
| RSynth | cascade + parallel, Klatt set |
| **klattsch** | **F1, F2, F3, parallel only** |

The bank format already anticipates this: `sequencer.js` scales `F4`/`BW4` and
up by the running `s` if a bank defines them, and the synth simply drops them.
So the data path for more formants exists and the DSP does not. Klatt's own
fricative spectra live in A3–A6 around 3–5 kHz, which is why the
`klatt1980-en` bank's own `source` field admits it moves F3 up into that band
to fake the hiss.

**Take from this:** the C engine should be built with a compile-time formant
count, defaulting to 3 so the port reproduces the original exactly, and
buildable at 5 or 6 once the goldens are locked. Not a rewrite later; a
dimension from the start.

## Nothing here is more primitive about the voice source

| Synth | Voiced source |
|---|---|
| DECtalk, Ibahu, Manatu | LF (Liljencrants–Fant) with Rd, plus jitter/shimmer/flutter |
| Wintalker | inverse-DFT buzz, dual glottal oscillators |
| Votraxxion | 9-level glottal pulse from the chip ROM |
| TruVoice | glottal waveform with an OQ-equivalent parameter |
| **klattsch** | **Rosenberg derivative, one knob, perfectly periodic** |

The survey names LF as the de facto standard for modern formant synths.
klattsch's pulse is a 12-line function with no stochastic component at all —
which is why it sounds mechanical in a specific, era-appropriate way, and is a
large part of the intended character. That is worth preserving as the default.

**Take from this:** jitter, shimmer and flutter are the cheapest naturalness
wins available, and all three are pure additions to the excitation stage that
cost nothing when set to zero. Ibahu and Manatu both treat them as stochastic
rather than LFO-driven; DECtalk uses a three-oscillator flutter. Implement as
optional parameters defaulting to 0.

## Nasal pole/zero is missing and the syntax already pretends it exists

DECtalk, Manatu and Wintalker all have a nasal pole and an anti-resonance.
klattsch has none — `M`, `N` and `NG` in the bank are ordinary three-formant
targets. Upstream's own documentation uses `[FNZ=450]` as the illustrating
example of an extended engine-specific directive, which is a nasal zero
frequency for an engine that does not exist yet.

**Take from this:** the C engine is that engine. `FNZ`/`FNP` implemented as a
real pole-zero pair, reachable through exactly the bracket syntax upstream
already documents, keeps source strings compatible in both directions.

## The text front end is the blocking gap, and it is already solved next door

For a screen reader this matters more than any DSP question. Compare:

| Synth | Text to phoneme |
|---|---|
| RSynth / SynthFix | CMU dictionary + English letter-to-sound rules |
| STSpeech | CMU dictionary + ported reciter |
| Votraxxion | NRL rules in C, compiled in, no data files |
| SAM | rule-based Reciter ported from the C64 original |
| TruVoice | exception dictionary, L2S rules, homographs, number/address/email normalization |
| **klattsch** | **CMU dictionary lookup, optional dependency, no fallback** |

`votraxxion/src/ttv.c` is the important one. Its stage 1 is
**text → ARPABET** via NRL rules, and its stage 2 is ARPABET → Votrax phone
codes. klattsch speaks ARPABET natively, so **stage 1 is directly reusable and
stage 2 is simply not needed.** It is C11, ~742 lines plus ~793 lines of
tables, allocates nothing, reads no file, and is already shipping inside an
NVDA add-on in this same folder under a license the author controls.

**Take from this:** do not write a front end. Lift `ttv.c` stage 1, strip stage
2, and keep its bounded-buffer discipline. This is the single largest
schedule saving available to the project. See [NVDA-ADDON.md](NVDA-ADDON.md).

## Duration is the other missing model

STSpeech carries duration per phoneme in its 34-byte table entries. Votraxxion
gets duration from the chip ROM. Manatu has locus equations and Hermite
smoothstep. RSynth blends elements by dominance rank. klattsch gives every
phoneme the same `rate` milliseconds and multiplies by 1.5 for stress.

That single number is why klattsch reads as machine-paced rather than merely
robotic. A stop and a diphthong occupying identical slots is not a stylistic
choice, it is an absence.

**Take from this:** add an optional `durationMs` field per phoneme in the bank
schema, defaulting to the current behaviour when absent. Bank schema version 2,
backward compatible, and `klatt1980-en` can gain durations from Klatt 1980's
own tables later without touching the engine.

## What klattsch does better than its neighbours

Worth saying plainly, because the port must not lose any of it.

- **The compiler/synth split.** A plain, serializable schedule of timestamped
  targets, with the synth as one of several possible consumers. Nothing else in
  the folder separates the two this cleanly. It is what makes independent
  verification of the two halves possible during the rewrite.
- **The bank format.** JSON, versioned, with `extends` inheritance and deletion
  by `null`. Manatu's 124-IPA Python data files are richer; klattsch's format is
  the more reusable one, and adding a language is a data change.
- **The directive language.** Compact, sticky, with relative and absolute forms,
  note names for pitch, syllable grouping and per-phoneme pitch ornaments. For
  hand-authoring prosody it is more expressive than anything else here.
- **Polyphony.** `[voice=N]` sections compiled independently and mixed. Unique
  in this collection.
- **Round-trippable output.** The WAV encoder embeds the source string in an
  `ICMT` chunk, so a rendered file says how to regenerate itself. Worth keeping
  in the C encoder.

## The decision this comparison settles

The survey's own recommendation section argues for modern C++ for new work, and
for C where footprint and toolchain reach matter most. This project takes the
second, for the reasons in [REWRITE.md](REWRITE.md) — and because the folder
already contains the experiment: `votraxxion` moved from header-only C++ to C11
and wrote down what it cost and what it bought. klattsch's engine is three
biquads and a bag of scalars. None of it wants a class.
