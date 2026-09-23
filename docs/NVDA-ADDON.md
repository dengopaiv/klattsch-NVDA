# The NVDA add-on

## What it has to be

One Python shim and one native library. No numpy, no scipy, no
pronunciation-dictionary package, no data files. The pattern is
`votraxxion/nvda-addon/`, whose add-on and GUI together ship in a few hundred
KB, after the Python add-on it replaced carried roughly 300 MB of vendored
wheels to do the same job.

**64-bit only.** One x64 library, `minimumNVDAVersion = 2026.1`. No x86 build,
no architecture switch in the shim, no fallback path. House rule §4 of
`..\CLAUDE.md` forbids producing a 32-bit version of anything, and dropping
32-bit NVDA 2025 and earlier is the intended consequence rather than an
oversight. ARM64 is added when it comes up.

## The blocking problem, and the answer

klattsch speaks phonemes. NVDA hands over text. There is no letter-to-sound
front end in the engine at all — `src/engine/pronounce.js` is a CMU dictionary
lookup behind an *optional* npm dependency, with no rule fallback, no number or
abbreviation handling and no homograph disambiguation. A word not in the
dictionary produces nothing.

`votraxxion/src/ttv.c` already solves this, in C, in this same folder:

```
text --TTV_NRL_RULES--> ARPABET --TTV_ARPABET--> Votrax phone codes
       ^^^^^^^^^^^^^^^^^^^^^^^^                  ^^^^^^^^^^^^^^^^^^
       exactly what klattsch needs               not needed at all
```

742 lines of matcher plus 793 lines of tables. Allocates nothing, opens no
file, needs no dictionary. **Stage 1 is lifted; stage 2 is dropped.** klattsch
consumes ARPABET natively, which is the format sitting between the two stages.

What has to be built on top of the lift:

- **Stress.** The NRL rules emit ARPABET without stress marks. klattsch's
  stress affects both duration (×1.5) and pitch (+8 Hz), so a stress
  assignment pass is needed. First cut: primary stress on the first full vowel
  of a content word, which is what most of this family of synth does.
- **Number, abbreviation and symbol normalization.** TruVoice is the model for
  how far this can go (addresses, phone numbers, email headers). The first
  version needs cardinals, ordinals, and the punctuation names a screen reader
  says aloud.
- **Sentence contour.** The compiler already has punctuation pauses and
  per-phoneme pitch deltas. A contour pass emits `.` as falling and `?` as
  rising by writing pitch deltas onto the last stressed vowel, which means it
  produces an ordinary klattsch source string and is testable without audio.

A compiled-in CMU dictionary is deliberately *not* in the first version.
It is 130,000 words, it is the difference between good and correct pronunciation,
and it is a size and licensing decision that should be made on measurements
rather than in advance. The rules-only front end ships first; the dictionary is
an exception layer added later, in front of the rules, if the rules prove
insufficient in real use.

## Layout

```
nvda-addon/
  manifest.ini
  package.py                       builds the x64 DLL, writes the .nvda-addon
  addon/
    synthDrivers/
      klattsch.py                  the shim
      klattschNative-x64.dll
    doc/en/readme.html
```

## The shim

Modelled directly on `votraxxion/nvda-addon/addon/synthDrivers/votraxNative.py`,
which is the working reference for every NVDA-side detail below.

- **One thread touches the engine.** NVDA's speech sequences become work items
  on a queue; a single worker thread renders and feeds `nvwave`. Nothing needs
  locking because nothing else calls into the library after startup.
- **Cancellation is an epoch counter.** `cancel()` bumps it; work items carrying
  an older epoch are dropped when the thread reaches them. Control items (rate,
  pitch, voice) carry no epoch, because a settings change must survive a cancel.
- **`IndexCommand` and `synthIndexReached`.** Index callbacks fire as the
  corresponding audio is handed to the wave player, which is what drives
  say-all and caret tracking. The schedule's `phrases` array already carries
  `tStartMs`/`tEndMs` per token, so index placement is a lookup, not an
  estimate — this is a place klattsch is better equipped than its neighbours.
- **`CharacterModeCommand`.** Spelling mode bypasses the front end and speaks
  letter names.
- **The output-device config key moved** between NVDA versions:
  `config.conf["audio"]["outputDevice"]` with a fallback to
  `config.conf["speech"]["outputDevice"]`.

## Settings exposed in NVDA's synthesizer panel

NVDA's standard settings map onto engine parameters directly:

| NVDA setting | Engine |
|---|---|
| Rate | `rate` (ms per phoneme), inverted and curved |
| Pitch | `baseF0` |
| Volume | `gain` |
| Inflection | scale factor on `stressF0Lift` and contour deltas |
| Voice | a named preset: a full parameter set, see below |

Plus driver settings beyond the standard set:

| Setting | Engine | Kind |
|---|---|---|
| Breathiness | `aspiration` | slider |
| Effort | `effort` | slider |
| Spectral tilt | `tilt` | slider |
| Head size | `scale` (formant frequency scale) | slider |
| Vibrato depth / rate | `vibratoDepth`, `vibratoRate` | sliders |
| Phoneme bank | bank name | list |
| Sample rate | 22050 / 44100 / 48000 | list |

**Rate deserves its own decision, and it is the one `votraxxion` got wrong
first.** There are two incompatible meanings of "faster": shortening phoneme
slots at constant pitch, and running the whole engine faster. klattsch's `rate`
directive is the first and is the right default — a screen reader user turning
the speed up does not expect the voice to rise. There is no reason to offer the
second here; the Votrax hardware had one clock knob and klattsch does not.

## Voices as presets

A "voice" in the NVDA sense is a named bundle of engine parameters: `baseF0`,
`scale`, `effort`, `aspiration`, `tilt`, vibrato, and optionally a bank. The
same preset format the GUI generator reads and writes — see
[GENERATOR.md](GENERATOR.md). A preset saved in the generator drops into the
add-on's preset directory and appears in NVDA's voice list. That round trip is
the point of building both.

## Packaging

`package.py` builds the x64 DLL, stamps the version and writes the
`.nvda-addon` (a zip). **The version lives in one place** — a constant in the
public C header — and everything that needs it parses that: CMake's
`project(VERSION)`, the add-on manifest, the GUI's `VERSIONINFO`, and the
release script. Three places that "move together" is three places that can
drift; one place cannot.

Releases carry a SHA-256 for every file. The build is unsigned, and a checksum
somebody can actually check is the only thing distinguishing it from any other
unsigned executable.

## Order

- [ ] **1.** Engine steps 0–6 in [REWRITE.md](REWRITE.md) green. Nothing here
      starts before the C engine renders correct WAV files from the CLI.
- [ ] **2.** Front end: lift `ttv.c` stage 1, drop stage 2, add stress
      assignment. Verified against a word list far larger than any test corpus,
      by diffing ARPABET streams — no audio needed.
- [ ] **3.** Normalization: numbers, abbreviations, punctuation names.
- [ ] **4.** Contour pass: sentence-final falling and rising.
- [ ] **5.** C API for the add-on: text in, audio out, cancel, settings.
      Exercised from a C test harness before Python sees it.
- [ ] **6.** The shim, against 64-bit NVDA 2026.1 and later, with
      [SCREEN-READER.md](SCREEN-READER.md) §2 as the acceptance checklist.
- [ ] **7.** `package.py`, x64 only, checksums.
