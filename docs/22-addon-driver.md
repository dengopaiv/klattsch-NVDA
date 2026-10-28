# The NVDA driver, and the add-on package

Steps 6 and 7 of [NVDA-ADDON.md](NVDA-ADDON.md). The driver is this project's
own, written from NVDA 2026.1's source and the requirements of
[SCREEN-READER.md](SCREEN-READER.md) §2, over the streaming API of
[21-stage-api.md](21-stage-api.md). The package is built by
`packaging/make-addon.py`.

**Exit tests:**

- **Without NVDA** (step 6): `tools/test-driver.py`, ctest `driver-stubs` —
  ✅ every check passes against the MSVC DLL and the WSL gcc `.so`,
  2026-10-28.
- **With NVDA running, by a person** (step 6): the checklist of §22.6 — ○
  not yet run.
- **The package installs into NVDA and its versions agree** (step 7): ◐ the
  package is built and every pre-pack check passes (§22.5); installing it is
  part of §22.6.

---

## 22.1 What was read in NVDA's source

NVDA's driver interface has moved between versions, so it was read from the
source of the minimum version, not written from memory: `nvaccess/nvda`, tag
`release-2026.1`, fetched 2026-10-28. The installed NVDA here is 2026.3 RC1,
which is what the hands-on test will run on. In-box drivers (`oneCore.py`,
`espeak.py`, `_espeak.py`) were read to see how NVDA uses its own
interface. They are GPL, and nothing was copied from them.

| File | What the driver takes from it |
|---|---|
| `synthDriverHandler.py` | `SynthDriver`; the setting factories, including `RateBoostSetting` and `InflectionSetting`; `VoiceInfo`; `synthIndexReached`, `synthDoneSpeaking`; `initSettings`, which applies a setting's default only where the driver has no attribute of that name |
| `autoSettingsUtils/driverSetting.py`, `utils.py` | `NumericDriverSetting` (0–100, default 50), `DriverSetting`, `StringParameterInfo` |
| `gui/settingsDialogs.py`, `synthSettingsRing.py` | A list setting's values come from `available{id.capitalize()}s`. `capitalize()` lowercases the rest, so the setting `sampleRate` needs a property named `availableSamplerates` |
| `nvwave.py` | `WavePlayer(channels, samplesPerSec, bitsPerSample, outputDevice)`; WASAPI only; `feed(data, onDone=...)` calls `onDone` when that chunk has *played*, from the audio side; `stop()` discards the callbacks still pending; `idle()` blocks until playback ends |
| `speech/commands.py` | `IndexCommand.index`, `CharacterModeCommand.state`, `BreakCommand.time` (ms), `PitchCommand.newValue` (absolute, on the 0–100 scale) |
| `speech/manager.py` | What happens to indexes after a cancel: §22.2 |

## 22.2 Two requirements of SCREEN-READER.md, refined by the source

Both were written from experience with older drivers, and both are right in
spirit. The source says what each means in NVDA 2026.1.

**§2.1, "an utterance that is abandoned — cancelled, failed, empty — still
fires its pending indices".** For a *failed* or *empty* utterance this
stands, and the driver does it: the speech manager is still waiting for
those indexes. For a *cancelled* one it does not apply. NVDA cancels
through `SpeechManager.cancel()`, which calls the synth's `cancel()` and
then `_reset()`, and that clears `_indexesSpeaking`. An index that arrives
afterwards goes to `_handleIndex`, which logs "Unknown index, speech
probably cancelled from main thread" and does nothing else. So reporting a
cancelled utterance's indexes would be harmless and pointless. NVDA's own
drivers don't: their `cancel()` is `player.stop()`, which throws the
callbacks away. Neither does this one.

**§2.2, "the audio thread must keep feeding even when idle".** This
described a player that checked its callbacks only when fed. The 2026.1
`WavePlayer` is WASAPI only, and its `onDone` callbacks come from the audio
side when a chunk finishes, whether or not anything more is fed. The
driver needs no periodic flush. What it does need is `idle()` once nothing
more is queued, then `synthDoneSpeaking`. It calls `idle()` only then, not
after every utterance, because `idle()` blocks until playback ends. NVDA's
oneCore driver makes the same choice.

Both refinements are recorded in SCREEN-READER.md beside the requirements.

## 22.3 The driver

`nvda-addon/addon/synthDrivers/klattsch.py`, one file, 14 KB.

- **One worker thread** owns the library handle and creates the player.
  `speak()` queues the sequence with the current generation and returns.
  `cancel()` increments the generation and stops the player. The worker
  checks the generation between 40 ms chunks.
- **Settings are values, not queued items.** The worker reads them at the
  start of each utterance, so a setting made just before a cancel survives
  it (§2.3) by construction.
- **Indexes** ride on the chunk that ends where the API placed them, as that
  chunk's `onDone`. The API stops every chunk at an index, so the placement
  is exact. An index that comes out with no audio of its own, such as one at
  the very start or two at one place, joins the chunk before it. That is
  why the worker holds one chunk back. With no chunk at all, it is notified
  at once.
- **`PitchCommand`** ends the compile being built and starts another at the
  new pitch. The front end resets every sentence to the compiler's base
  pitch, so a pitch change inside one compile would be undone at the next
  sentence.
- **Character mode** spells ASCII through `kl_api_spell`. Anything else goes
  through the front end, which folds accented letters, because
  `kl_text_spell` skips non-ASCII.
- **Failure**: an exception inside an utterance is logged, the utterance's
  unreported indexes are notified, and the worker carries on. Speakable text
  (letters or digits) that compiles to no sound is logged as an error with
  the compiler's warnings: the "silent success" of SCREEN-READER.md §4.
- **The library** is refused unless the installed add-on's manifest version
  (`0.5.0-beta`) begins with the library's `kl_api_version()` (`0.5.0`).

The slider curves are the stand-ins of NVDA-ADDON.md, written as functions
at the top of the file. With every slider at its default the driver's
settings are the engine's defaults exactly: 110 ms, 120 Hz, inflection 1,
volume 1, scale 1.25⁰ = 1, tilt 0, effort 0.5, vibrato 0 at 5 Hz. That is
what lets §22.4 compare its audio with the API byte for byte.

## 22.4 The test without NVDA

`tools/test-driver.py <library>` writes stand-ins for the NVDA modules the
driver imports into a temporary directory, copies the driver and the real
library beside them, and drives it. The stand-ins model §22.1 and nothing
more. The wave player plays on its own thread at 20× real time, calls each
chunk's `onDone` after that chunk has played, and on `stop()` discards what
is pending. Settings are auto-properties. `initSettings` touches only
attributes the driver lacks.

What it checks, 2026-10-28, all passing on both libraries:

| | |
|---|---|
| Two sentences with indexes | the audio fed is the API's, **byte for byte** (185,000 bytes); indexes 1, 2 once each, in order; then done |
| No audio | an index alone; empty and blank text with two indexes |
| Punctuation only | `...` is 1.05 s of silence (three 300 ms pauses and the trail-off), then its index |
| Commands | a 300 ms break adds at least 290 ms; character mode speaks; `PitchCommand` changes the audio and `PitchCommand()` restores it exactly |
| Cancel | after a cancel the next utterance speaks and its index comes out; the cancelled indexes are not reported; a rate set before the cancel applies after it |
| Failure | an exception on the second render: both indexes and done are still notified, the error is logged, and the next utterance speaks |
| Silent success | text with letters that yields no sound is logged as an error |
| Settings | a new sample rate makes a new player; 8,000 Hz is refused; volume 0 gives silence of the right length |
| `terminate()` | the worker ends |

**What its first runs corrected.**

- *The stand-in, not the driver.* The first stand-in applied every
  setting's default at startup, which set volume to 50, and the audio
  failed to match the API's. Reading `initSettings` again showed that NVDA
  applies a default only where the driver has no attribute, and on first
  load saves the driver's own values. The stand-in was corrected to do the
  same. The driver's default volume is 100.
- *Chapter 21.* The test fed `...` expecting no audio, and got 1.05 s of
  silence. Chapter 21 had said the punctuation-only corpus lines render
  nothing. Three of them render pauses, and §21.4 is corrected.

It runs in ctest wherever Python is found, so CI runs it against the
Linux build of the same C.

## 22.5 The package

`python packaging/make-addon.py --tested-on <NVDA version> [--pretest]`.
Before packing, it refuses on any of these:

- `kl_version.h` disagrees with itself;
- the tree is not clean;
- the DLL is not x64 (its PE machine field);
- the DLL's own `kl_api_version()` is not the header's (loaded with
  `ctypes`);
- `kl_api_test stream` or `api` fails on the build;
- `tools/test-driver.py` fails against the very DLL being packed.

`--tested-on` becomes `lastTestedNVDAVersion`, so the manifest names what
was tested. `--pretest` marks a build made *for* the first hands-on test,
in its file name and its readme.

First build, 2026-10-28, commit `251db82`:
`dist/klattschNative-0.5.0-beta-pretest.nvda-addon`, 159,258 bytes, SHA-256
`15586e20…030b7a`, and every check passed. Its contents: `manifest.ini`
(version `0.5.0-beta`, minimum 2026.1.0, last tested 2026.3.0),
`synthDrivers/klattsch.py`, `synthDrivers/klattschNative.dll`,
`doc/en/readme.html`, `LICENSE.txt`, `NOTICE.md`.

The version and clean-tree checks moved into `packaging/kl_release.py`,
which `make-dist.py` now imports as well. `make-dist.py` was rerun after the
move and built the generator beta as before.

## 22.6 The hands-on test — a person's, with NVDA running

On NVDA 2026.3 (64-bit), with the pretest package. Each line gets a result
here.

1. ○ The package installs; NVDA shows Klattsch Native 0.5.0-beta in the
   add-on store's installed list; after the restart it is in the
   synthesizer list (NVDA+Control+S) and can be chosen.
2. ○ Ordinary use: arrowing through a document line by line, by word, by
   character; tabbing through a dialog. Speech starts at once and stops at
   once when interrupted.
3. ○ Say-all (NVDA+Down Arrow) over a long document: it reads to the end
   without stalling, and the caret follows.
4. ○ Cancel under load: start say-all, press Control, start again, several
   times quickly. No speech from before the cancel leaks into what follows.
5. ○ Spelling: NVDA+Up Arrow pressed twice and three times. Capitals are
   spoken higher, the PitchCommand path.
6. ○ The settings ring (NVDA+Control+arrows): rate, rate boost, pitch,
   inflection, volume, breathiness, effort, head size. Each is reachable,
   and each step is audible.
7. ○ Voice settings dialog (NVDA+Control+V): every control is labelled and
   reachable from the keyboard, including the sample rate list.
8. ○ The slider curves: where each one stops being useful at either end.
   This replaces the stand-ins.
9. ○ Anything else heard, especially the voice itself, the stress, the
   sentence melody, and the 150 ms of silence the compiler leaves after
   every utterance.

When 1–7 pass, the package is rebuilt without `--pretest` for testers, with
`--tested-on` naming the NVDA version used.
