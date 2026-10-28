# The NVDA add-on

## What it has to be

One Python driver and one native library. No numpy, no scipy, no
pronunciation-dictionary package, no data files. Everything the add-on does is
in `klattschNative.dll`, which is the engine, the front end and a thin
streaming API built from the pieces this repository has already verified. The
driver only translates between NVDA and that API.

**Planned from klattsch alone** (decided 2026-10-28). The driver is this
project's own, written against NVDA's documented driver interface and the
requirements in [SCREEN-READER.md](SCREEN-READER.md). It is not adapted from
another synthesizer's add-on. That keeps its provenance as simple as the rest
of the repository: Tony Gies's MIT notice for the engine, the BSD-3-Clause
notices of [NOTICE.md](../NOTICE.md) for the text front end, and nothing else.
NVDA's own in-box drivers may be read to confirm how the interface behaves,
but they are GPL, so nothing is copied from them.

**64-bit only.** One x64 library, `minimumNVDAVersion = 2026.1`. No x86 build,
no architecture switch in the driver, no fallback path. House rule §4 of
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
- **Number, abbreviation and symbol normalization.** Commercial screen-reader
  engines take this a long way — addresses, phone numbers, email headers — and
  that is the standard to measure against eventually. The first version needs
  cardinals, ordinals, and the punctuation names a screen reader says aloud.
- **Sentence contour.** The compiler already has punctuation pauses and
  per-phoneme pitch deltas. A contour pass emits `.` as falling and `?` as
  rising by writing pitch deltas onto the last stressed vowel, which means it
  produces an ordinary klattsch source string and is testable without audio.

> **Done 2026-09-25**, in [19-frontend-text.md](19-frontend-text.md):
> `csrc/kl_text.c`, text in, klattsch source out. Three things above turned
> out differently when measured, and are corrected here rather than edited
> away:
>
> - **"Primary stress on the first full vowel"** was the first-cut plan.
>   Measured against the CMU dictionary on ordinary vocabulary, "the first
>   vowel" is right for **60.9 %** of words of two or more syllables. Suffix,
>   prefix and default rules, each measured before it was kept, reach
>   **82.4 %**.
> - **"The punctuation names a screen reader says aloud"** are not the
>   synthesizer's job. NVDA's symbol processing replaces symbols according
>   to the user's punctuation level before the text arrives; a synthesizer
>   that named them too would name some twice, or name the ones the user
>   asked not to hear. The front end names none.
> - **The contour** writes more than the last stressed vowel: a reset and
>   start level per sentence, a declination across it, a sticky fall on the
>   nucleus so the tail stays low, and a continuation rise before a comma.
>   Its sizes are stand-ins, not measurements, and are marked as such.
>
> The lift itself was checked, not assumed: the letter-to-sound pass gives
> Votraxxion's own output, byte for byte, on a 36-line hand corpus and on all
> 124,076 plain words of the CMU dictionary.

A compiled-in CMU dictionary is deliberately *not* in the first version.
It is 130,000 words, it is the difference between good and correct pronunciation,
and it is a size and licensing decision that should be made on measurements
rather than in advance. The rules-only front end ships first; the dictionary is
an exception layer added later, in front of the rules, if the rules prove
insufficient in real use.

> **2026-10-28:** the measurements say they do (36 % of ordinary words exactly
> right). How four other engines in this tree built theirs, and what that means
> here, is a proposal in [DICTIONARY.md](DICTIONARY.md).

## Layout

```
csrc/klattsch_api.h, kl_api.c      the streaming API (step 5)
csrc/tools/kl_api_test.c           its C harness, in ctest
nvda-addon/
  manifest.ini.in                  the manifest, version left blank
  addon/
    synthDrivers/
      klattsch.py                  the driver
      (klattschNative.dll)         never in the tree: the packager writes it
                                   straight into the package
    doc/en/readme.html             what it is, who made it, the settings;
                                   a template the packager stamps
tools/test-driver.py               the driver under stub NVDA modules
packaging/make-addon.py            builds the .nvda-addon into dist/
```

One library, so its name carries no architecture suffix. The packager sits
beside `packaging/make-dist.py` and shares its version and clean-tree checks,
instead of each script keeping its own copy.

## The C API — `csrc/klattsch_api.h`

What exists is a set of verified pieces, and the generator strings them
together for a whole utterance: `kl_text_to_source`, `kl_tokenize`,
`kl_compile`, `kl_render_mix`, `kl_wav_encode`. That is right for a window that
saves files, and wrong for a screen reader in two ways. It renders everything
before the first sample can play, which is the 1-second stall of
[SCREEN-READER.md](SCREEN-READER.md) §3. And it has no notion of an NVDA index.
The API adds those two things and nothing else.

```c
typedef struct kl_api kl_api;                    /* opaque; one per thread */

KL_API const char *kl_api_version(void);         /* KL_VERSION_STRING */
KL_API kl_api *kl_api_create(int sample_rate);   /* NULL on failure */
KL_API void    kl_api_destroy(kl_api *h);

/* Settings, applied from the next kl_api_begin(). */
KL_API int    kl_api_set(kl_api *h, int setting, double value);  /* KL_SET_* */
KL_API double kl_api_get(const kl_api *h, int setting);

/* Building one utterance, piece by piece, in NVDA's order. */
KL_API void kl_api_begin(kl_api *h);
KL_API int  kl_api_text(kl_api *h, const char *utf8);    /* through the front end */
KL_API int  kl_api_spell(kl_api *h, const char *utf8);   /* character mode */
KL_API int  kl_api_source(kl_api *h, const char *src);   /* raw klattsch source */
KL_API int  kl_api_break(kl_api *h, int ms);             /* becomes p<ms> */
KL_API int  kl_api_index(kl_api *h, int index);
KL_API long kl_api_end(kl_api *h);   /* compiles: total samples, 0 if silent, <0 error */

/* Rendering. Writes at most `cap` samples and stops early at an index, so
 * the caller can attach the index to exactly the audio before it. *index is
 * the index reached at the end of this chunk, or -1. Returns the samples
 * written; 0 with *index == -1 means the utterance is finished. Indexes left
 * over at the end come out one per call, with 0 samples. */
KL_API int kl_api_render(kl_api *h, int16_t *out, int cap, int *index);
```

Only pointers, ints and doubles cross the boundary, so `ctypes` needs no
structures and no callbacks. Nothing in the library calls back into Python.

- **5a. Chunked rendering.** The handle holds one `kl_synth` per voice section
  and renders the chunk it is asked for. `kl_synth_process` already keeps its
  state across calls (`stage3-chunked` proves it), so this is a new loop, not
  a DSP change. Voices are summed per chunk in float, as `kl_render_mix` sums
  them, so the stream equals its output sample for sample. The front end emits
  only voice 0. `[voice=N]` in raw source is still honoured.
- **5b. Level.** The WAV path scales the whole file to a peak of 0.95, and the
  generator plays that file. A stream cannot know its peak before its end, so
  the stream is the soft-clipped float × a fixed output gain × the volume
  setting, rounded to 16 bits and clamped. The fixed gain is measured over the
  text corpus so that the add-on's typical level matches the generator's. It
  goes in the chapter as a number, with how it was measured.
- **5c. Indexes.** Each index is recorded at the source offset where the
  pieces before it end. After `kl_compile`, an index's time is the `t_end_ms`
  of the last phrase whose `src_end` is at or before that offset, or 0 when
  there is none. That is the lookup §1 of SCREEN-READER.md promises, and it is
  exact: the compiler knows when every token will sound before a sample is
  rendered. Every index comes out of `kl_api_render` exactly once, in order,
  including the ones after the last sound and the ones in an utterance with
  no sound at all.
- **5d. Contour across pieces.** Each text piece goes through its own
  `kl_text_to_source` call, so the sentence contour restarts at an index that
  falls mid-sentence. Say-all places indexes at line and sentence ends, so
  this is rarely heard. It is a known limit of the first version, written
  down rather than hidden. The fix, if listening asks for it, is a marker the
  front end passes through.
- **5e. Settings** (`KL_SET_*`): rate in ms per phoneme, base F0, inflection,
  volume, aspiration, effort, tilt, scale, vibrato depth and rate, comma pause.
  All but two map onto `kl_compile_opts` or `kl_text_opts`, which exist.
  Volume is the post-scale of 5b. Inflection is a new `kl_text_opts` field
  that scales the front end's contour deltas; at its default of 1.0 the front
  end's output must stay byte-identical to `goldens/text`, which the existing
  `text-frontend` test proves.
- **5f. Storage and bounds.** Allocated by `kl_api_create`. The per-utterance
  buffers are sized by `kl_token_need` and `kl_compile_need` and only grow, so
  a warm handle allocates nothing on the speech path. ~~Text longer than
  `KL_TEXT_MAX` is split at sentence ends before the front end sees it.~~
  *Corrected 2026-10-28:* `kl_text_to_source` already hands each sentence on
  at its terminator and continues a run with none past the buffer rather
  than dropping it, so nothing was added ([21-stage-api.md](21-stage-api.md)
  §21.8). `kl_api_end`
  reports 0 when the utterance has no sound. The driver decides whether that
  is legitimate (index-only, punctuation-only) or a fault.
- **5g. The library itself.** `KL_API` export macro. A CMake `SHARED` target,
  `klattschNative`, linked against the static runtime (`MultiThreaded`, as
  `klattsch_mt` already is for the generator), so no VC++ redistributable is
  needed. x64 only.

## The driver — `nvda-addon/addon/synthDrivers/klattsch.py`

A `synthDriverHandler.SynthDriver` subclass named `klattsch`, shown as
"Klattsch Native". Each rule below exists because of one requirement of
[SCREEN-READER.md](SCREEN-READER.md) §2, cited.

- **One worker thread owns the library handle and the wave player.** NVDA's
  calls (`speak`, `cancel`, `pause`, setting changes) only put items on a
  queue. Nothing else calls into the library after startup, so nothing needs
  a lock.
- **Generations (§2.3).** `cancel()` increments a counter and stops the
  player. Every speech item carries the generation it was queued under, and
  the worker drops a stale item, or stops rendering one, between chunks.
  Setting items carry no generation, so a change made just before a cancel
  survives it.
- **Speech sequences.** The worker walks NVDA's sequence. A string goes to
  `kl_api_text`, or to `kl_api_spell` while a `CharacterModeCommand` is on. An
  `IndexCommand` goes to `kl_api_index`, a `BreakCommand` to `kl_api_break`. A
  `PitchCommand` (NVDA's raised pitch for capitals) ends the utterance being
  built and starts the next with the offset applied. The front end resets the
  contour to the compiler's base pitch, so a pitch change inside one compile
  would be undone at the next sentence. Commands the driver does not declare,
  such as `LangChangeCommand` (English only), are skipped.
- **Indexes (§2.1).** Audio is fed in the chunks `kl_api_render` returns. A
  chunk that ends at an index is fed with an `onDone` callback that notifies
  `synthIndexReached`, so the index fires when that audio has played. After
  the last chunk the worker waits for the player to go idle, then notifies
  `synthDoneSpeaking`. On a cancel, a failure or an empty utterance, every
  index not yet reported is notified anyway, then `synthDoneSpeaking`.
- **Idle (§2.2).** When the queue is empty, the worker still wakes on a short
  timeout and lets the player go idle, so the callbacks of the last chunk
  always run.
- **Failure (§4).** The worker's loop is wrapped so that an exception logs,
  reports the pending indexes and done, and keeps the thread alive. If the
  thread does exit, it signals it and leaves a sentinel. Speakable text that
  compiles to no sound is logged as an error, not passed over.
- **Sample rate.** A change recreates the library handle and the player on
  the worker thread, between utterances.

What the driver needs from NVDA — the setting classes, `WavePlayer`'s
constructor and its `feed`/`idle`/`stop`/`pause`, how the output device is
chosen, and the notifications — is read from the source of NVDA 2026.1 when
step 6 starts, and the chapter records what was read. These details have moved
between NVDA versions. The driver targets one documented version, not a
remembered one.

## Settings exposed in NVDA's synthesizer panel

NVDA's standard settings map onto engine parameters directly:

| NVDA setting | Engine |
|---|---|
| Rate | `rate` (ms per phoneme), inverted and curved |
| Pitch | `baseF0` |
| Volume | a scale on the output samples, after the engine |
| Inflection | scale factor on the front end's contour deltas |
| Voice | a named preset: a full parameter set, see below |

> **Corrected 2026-10-28**, by reading the compiler rather than assuming.
> Two rows of this table said what the engine cannot do as it stands.
>
> - **Volume was `gain`.** `gain` is one of the 19 synthesis parameters, but
>   nothing in klattsch source can set it: the compiler has no `gain` option
>   (`kl_opt` in `csrc/kl_compile.h`, `compileSection` in `sequencer.js`), and
>   a `[gain=4]` bracket is lower-case, so it is an unknown directive rather
>   than an extra. Only the synth's initial target reaches it. It also sits
>   *before* the soft clip at ±0.85, so raising it would change the timbre,
>   not only the level. Volume is therefore a multiply on the finished
>   samples, which leaves every golden untouched.
> - **Inflection scaled `stressF0Lift`.** That is `KL_DEFAULT_STRESS_F0_LIFT`,
>   a frozen 8 Hz in the compiler (`kl_compile.c`, `render_phoneme`). Scaling
>   it is an engine change, which the decision of 2026-09-25 postpones. The
>   contour deltas are the front end's own (`kl_text.c`, pass 4), so
>   inflection scales those, through a new `kl_text_opts` field, and the
>   stress lift stays 8 Hz until phase 3.

Plus driver settings beyond the standard set:

| Setting | Engine | Kind |
|---|---|---|
| Breathiness | `aspiration` | slider |
| Effort | `effort` | slider |
| Spectral tilt | `tilt` | slider |
| Head size | `scale` (formant frequency scale) | slider |
| Vibrato depth / rate | `vibratoDepth`, `vibratoRate` | sliders |
| Sample rate | 22050 / 44100 / 48000 | list |

> **Two decisions behind that table, recorded 2026-10-28.**
>
> **No phoneme bank list.** The front end writes ARPABET for the
> `klatt1980-en` bank. The two Japanese banks have six symbols each (five
> vowels and the tap `DX`), so English text through either is all unknown
> phonemes. The compiler gives an unknown phoneme no time at all and only
> warns, so the utterance would be silent while every call succeeds. That is
> the first failure mode of [SCREEN-READER.md](SCREEN-READER.md) §4. A bank
> becomes a setting when a front end exists for its language. A preset may
> still name a bank, and the loader refuses a bank the front end cannot
> drive.
>
> **No rate below 16 kHz.** The biquads clamp every formant to 0.45·fs
> ([12-stage0-goldens.md](12-stage0-goldens.md) §12.1), and `klatt1980-en`
> puts /s/ on its third band alone, at 5,500 Hz. Below about 12.2 kHz the
> clamp moves /s/ down towards /ʃ/ (1,840 and 2,750 Hz). The spectral peak of
> a sustained /s/, rendered by the frozen JavaScript engine on 2026-10-28,
> was **3,605 Hz at 8 kHz**, 4,950 Hz at 11,025 Hz, and 5,586 Hz at 16 kHz.
> The /ʃ/ peak stayed between 2.6 and 2.8 kHz at every rate. 3.5 to 4 kHz is
> where listeners put the /s/–/ʃ/ boundary, so at 8 kHz the contrast is
> mostly gone. The generator still offers 8 and 11.025 kHz, which is right
> for a tool that studies the engine and wrong for a screen reader. The head
> size slider raises the floor (5,500·s / 0.45), and 22,050 Hz holds /s/ up
> to s = 1.8.

**Rate deserves its own decision.** There are two incompatible meanings of
"faster": shortening phoneme slots at constant pitch, and running the whole
engine faster. klattsch's `rate` directive is the first, and is the only one
offered. A screen-reader user turning the speed up does not expect the voice
to rise. `RateBoostSetting` is a second multiplier on the same `rate`, not a
separate mechanism (SCREEN-READER.md §2.5).

### From NVDA's sliders to the engine

NVDA's settings run 0–100. The curves below are **stand-ins**, chosen so that
50 is the engine's default and so that every step is audible. Each is replaced
by a listened-to curve before the first tester build, and the chapter records
the listening.

| Setting | 0 | 50 | 100 | Shape |
|---|---|---|---|---|
| Rate (ms per phoneme) | 220 | 110 | 35 | exponential in the slider |
| Rate boost | — | ×1 | ×2 speed when on | divides the ms |
| Pitch (base F0, Hz) | 60 | 120 | 240 | exponential: an octave each way |
| Inflection (contour scale) | 0 | 1.0 | 2.0 | linear |
| Volume | silent | — | full | linear on the post-scale |

The compiler shrinks transitions with the slot (`min(35, 0.4·slot)` ms) and
caps a stop's release at 30 % of it, so a short slot stays well-formed rather
than overlapping. How fast it stays intelligible is a listening question,
which sets the 100 end.

In the settings ring (`availableInSettingsRing=True`): rate, pitch, volume,
inflection, voice, breathiness, effort, head size. That is the first list of
SCREEN-READER.md §5 without the phoneme bank. Tilt, vibrato and the sample
rate are in the panel only.

## Voices as presets

A "voice" in the NVDA sense is a named bundle of engine parameters: `baseF0`,
`scale`, `effort`, `aspiration`, `tilt`, vibrato, and optionally a bank. The
same preset format the GUI generator reads and writes — see
[GENERATOR.md](GENERATOR.md). A preset saved in the generator drops into the
add-on's preset directory and appears in NVDA's voice list. That round trip is
the point of building both.

**The first add-on has one voice, "Default".** The preset format is not built
yet (GENERATOR.md step 2), and it is built once, in C, for the CLI, the
generator and the add-on alike. A Python-only loader in the driver would be a
second implementation of one format, which is how the two drift apart. The
voice list grows when that loader exists, and the loader refuses a preset
naming a bank the front end cannot drive.

## Packaging — `packaging/make-addon.py`

Built the way `packaging/make-dist.py` builds the generator beta, and sharing
its checks (moved into one module both import):

- the version is `csrc/kl_version.h`'s, and its three spellings there agree;
- the working tree is clean, and the commit goes into the readme;
- the DLL it packs reports the same version through `kl_api_version()`, and
  passes `kl_api_test` — the file shipped is the file tested;
- `manifest.ini` is written from `manifest.ini.in`: name `klattschNative`,
  summary "Klattsch Native", the version, `minimumNVDAVersion = 2026.1.0`,
  `lastTestedNVDAVersion` = the NVDA version step 6's test ran on, and a
  description that names Tony Gies's klattsch as the engine;
- `LICENSE` and `NOTICE.md` go inside the package, since MIT and BSD-3-Clause
  both require their notices to travel with a binary.

It writes `dist/klattschNative-<version>-beta.nvda-addon` and its SHA-256.
`dist/` is ignored by git. A beta goes to testers, not to a GitHub release.
The build is unsigned, and a checksum somebody can actually check is the only
thing distinguishing it from any other unsigned file.

## Order

One branch per step, a numbered chapter per step, and `main` takes a step only
when its exit test passes.

- [x] **1.** Engine steps 0–6 in [REWRITE.md](REWRITE.md) green. Nothing here
      starts before the C engine renders correct WAV files from the CLI.
      **Done 2026-09-24** ([18-stage6-wav.md](18-stage6-wav.md)).
- [x] **2.** Front end: lift `ttv.c` stage 1, drop stage 2, add stress
      assignment. Verified against a word list far larger than any test corpus,
      by diffing ARPABET streams — no audio needed. **Done 2026-09-25**: the
      lift equals Votraxxion on all 124,076 CMU words; stress 82.4 % on
      vocabulary. [19-frontend-text.md](19-frontend-text.md).
- [x] **3.** Normalization: numbers, abbreviations, punctuation names.
      **Done 2026-09-25**, with punctuation names left to NVDA — see the
      correction above.
- [x] **4.** Contour pass: sentence-final falling and rising. **Done
      2026-09-25**; its sizes are stand-ins to be fitted later.
- [x] **5.** The C API — chapter 21, branch `stage-api`. **Done
      2026-10-28**, [21-stage-api.md](21-stage-api.md): all five parts pass
      on four toolchains; the output gain was measured as 1.0; 22 mutations,
      18 caught and 4 surviving for stated reasons.
      **Exit test**, in `ctest` and in `tools/build-matrix.ps1` on all four
      toolchains: (1) the chunked stream equals `kl_render_mix`'s float output
      for every corpus case at three chunk sizes, before the level of 5b is
      applied; (2) every index comes out exactly once and in order, including
      index-only and empty utterances and a cancel at every chunk boundary;
      (3) the corpus's speakable text never compiles to zero samples;
      (4) `text-frontend` unchanged with inflection at 1.0; (5) the C engine's
      time to first chunk and its real-time ratio, measured and recorded —
      SCREEN-READER.md §8 A has these for the JavaScript only. Then the fixed
      output gain of 5b, measured.
- [x] **6.** The driver — chapter 22, branch `addon-driver`. **Done
      2026-10-28** ([22-addon-driver.md](22-addon-driver.md)): tested without
      NVDA on both libraries, then installed and used in NVDA 2026.3 and
      reported working. First, the
      NVDA 2026.1 interface read from its source and written down. Then two
      exit tests. **Without NVDA:** `tools/test-driver.py` loads the driver
      under stub `synthDriverHandler`, `nvwave` and `speech.commands` modules
      and checks, against the real library, that every index is notified once and
      in order, that `synthDoneSpeaking` follows every utterance (cancelled,
      empty and failed ones included), that a setting survives a cancel, and
      that a raised exception leaves the worker alive. The library path is
      an argument, so the same test runs in CI against the Linux build of
      the same C.
      **With NVDA running**, by a person: say-all over a long document, cancel
      under load, the settings ring, spelling, capitals, every setting from
      the keyboard — the §8 E list of SCREEN-READER.md. The slider curves
      above are fitted in the same sitting.
- [x] **7.** `packaging/make-addon.py` — in chapter 22 (§22.5) rather than a
      chapter 23, and on the same branch: the hands-on test needs the
      package, so the two are finished together. **Done 2026-10-28**: the
      pretest package installed and worked; the tester package is rebuilt
      without `--pretest` (§22.7).
      **Exit test:** the package installs into a clean NVDA 2026.1 x64; the
      version NVDA shows, the manifest's, the DLL's and the header's are the
      same; the add-on speaks; the SHA-256 matches.
- [ ] **8.** Voices from presets, once GENERATOR.md step 2 has built the
      shared loader in C. **Exit test:** a preset saved by the generator
      appears in NVDA's voice list and renders, in the add-on, the same
      samples the generator plays for it (before the level of 5b).

Steps 5 to 7 change no sample that the goldens pin. The engine, the front end
and the generator are finished, and this is the layer between them and NVDA.
