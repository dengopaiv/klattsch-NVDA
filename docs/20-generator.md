# The sample generator, first version — a window to hear the engine in

Phase 4 of [ROADMAP.md](ROADMAP.md), cut down on purpose. The plan in
[GENERATOR.md](GENERATOR.md) is a tool that exposes every parameter
including the constants upstream froze; this first version is the part of it
that makes the synthesizer *listenable* without a command line, and nothing
the engine does not already have.

Two decisions of 2026-09-25 shaped it:

- **Ship the engine as it is.** The frozen constants stay frozen, so the
  window exposes what the compiler takes today: ten voice settings, the
  phoneme bank and the sample rate.
- **The GUI before the add-on**, so that the first listening can happen here.
  That moved GENERATOR.md's last step — text input, with the phoneme source
  visible and editable — to the front: typing a sentence is the point.

**Exit test:** `tools/verify-gui.mjs` (ctest `gui-render`) — the real
executable's render path byte-identical to the JavaScript reference over 24
cases (26 since the comma box); `tools/gui-mutations.py` catching all 11 of its mutations; and
`tools/check-gui-a11y.ps1` — 21 tab stops (20 before the comma box), every one named by its label, Tab
round the window with no trap, Escape not closing it. ✅ Passes on MSVC and
clang-cl. The listening test with NVDA running is a person's, and is the next
step (§20.6).

---

## 20.1 What it is

`gui-native/klattsch_gui.cpp`, one Win32 window, C++17, linked against the
same engine sources as everything else. One executable, `klattsch_gui.exe`,
that on MSVC needs nothing but DLLs every Windows has — measured with
`dumpbin /dependents`: `KERNEL32`, `USER32`, `GDI32`, `COMCTL32`, `COMDLG32`,
`WINMM`, `SHELL32`. The C runtime is linked statically; the library the
verified tools use keeps the default runtime, so the GUI gets its own
static-runtime build of the same sources (`klattsch_mt` in CMake).

C++ in a repository whose engine is C17 is the case GENERATOR.md and the
tree's rules both allow: a GUI, in its own binary, touching the C library
only through its headers.

In tab order:

| | Control | Notes |
|---|---|---|
| 1 | **Text to speak** (Alt+T) | multi-line; English text, or phoneme source in phoneme mode |
| 2 | **Phoneme mode** (Alt+H) | the box holds klattsch source rather than text |
| 3 | **Phoneme bank** (Alt+B) | the three compiled-in banks, by display name |
| 4 | Sample rate | 8000 to 48000 Hz; 48000, the CLI's, by default |
| 5–14 | Base pitch (Alt+P), Rate (Alt+R), formant scale, vibrato depth and rate, tremolo depth and rate, aspiration, spectral tilt, effort | spin boxes |
| 15 | Comma pause (ms) | a front-end setting, not a compiler option: text mode only; 200 by default (added after the first listening) |
| 16 | **Speak** (Alt+K; Enter anywhere but the text box) | renders on a worker thread, then plays |
| 17 | **Stop** (Alt+O, and Escape) | |
| 18 | **Convert to phonemes** (Alt+C) | replaces the text with the front end's source and turns phoneme mode on — the way to see and edit what it made of a sentence |
| 19 | **Save WAV** (Alt+W) | with the spoken source in the file's ICMT chunk |
| 20 | **Reset defaults** (Alt+D) | |
| 21 | **Messages** (Alt+M) | read-only: length, sample rate, compiler warnings, and in text mode the phoneme source that was spoken |

The settings are spin boxes, which hold integers, so fractional settings are
shown in a unit that makes them whole — scale, tremolo depth, aspiration,
tilt and effort in percent. Every default converts exactly (50 × 0.01 is the
double 0.5), so an untouched window gives the same numbers the engine uses on
its own, and the same samples as the CLI.

Messages go into a box rather than message boxes, so that speaking never
takes focus away and a warning can be read again line by line.

---

## 20.2 The engine path, and how it was proved

The window calls exactly what `bin/klattsch_cli.c` calls — tokenize, compile,
render every voice and mix, encode with peak normalization — with the ten
settings passed as compiler options, the bank by name and the sample rate
chosen. In text mode `kl_text_to_source()` runs first, with its contour sized
from the base pitch in the window.

The window itself cannot be driven from a test, so the executable has a
headless mode that runs the same functions its buttons run:

```
klattsch_gui.exe --selftest OUT.WAV PHONEMEMODE BANK SAMPLERATE P1 ... P10 TEXT
```

`tools/verify-gui.mjs` drives the real executable through it and compares
each WAV byte for byte with the JavaScript engine rendering the same source
with the same options, as `bin/klattsch.mjs` renders. For text-mode cases the
source comes from `kl_text_dump --source --base HZ` (the front end, verified
separately in [19-frontend-text.md](19-frontend-text.md)), and the verifier
also checks that the generator wrote that same source into the WAV's ICMT
chunk.

24 cases: the defaults in both modes; each of the ten settings off its default
alone, then all of them at once; the text contour at a high base pitch; both
Japanese banks; every sample rate the window offers; a two-voice source; a
source with a compiler warning; text outside ASCII. **All 24 byte-identical.**
The three banks were also confirmed to give three different files for the
same phonemes, since two same-sized files had made that worth checking.

`tools/gui-mutations.py` then broke the path ten ways — a wrong unit, two
settings wired to each other's option, no option marked present, the bank
ignored, the sample rate fixed at 48000, the contour sized for 120 Hz whatever
the pitch, phoneme mode inverted, the source left out of the WAV, no
normalization, the self-test reading its arguments one early. **10 of 10
caught.**

---

## 20.3 Accessibility, checked by machine where a machine can

`tools/check-gui-a11y.ps1` launches the window and asks what a screen reader
would find, without sending one keystroke to the desktop:

1. **Tab order**, walked with `GetNextDlgTabItem` — the function
   `IsDialogMessage` calls when Tab is pressed — from the text box round to
   itself.
2. **Names and roles** of each stop, through MSAA (`AccessibleObjectFromWindow`),
   which is how NVDA reads standard Win32 controls. A Win32 edit or combo has
   no name of its own; it takes the label created just before it, so this is
   where a control made in the wrong order would show.
3. **Keyboard traps**: each multi-line box is sent the real `WM_GETDLGCODE`
   with a Tab and with an Escape keydown and must not claim either; and an
   Escape is posted into the text box, through the window's own message
   loop, after which the window must still be open.

Result:

```
 1. editable text  Text to speak:
 2. check box      Phoneme mode (the box holds klattsch phoneme source)
 3. combo box      Phoneme bank:
 4. combo box      Sample rate (Hz):
 5. editable text  Base pitch (Hz):
 ...
14. editable text  Effort (%):
15. push button    Speak
 ...
19. push button    Reset defaults
20. editable text  Messages:
```

20 stops, every one named by its label, Tab round the window, no trap.

### What it found

**Escape in the text box closed the program.** Given Escape, a multi-line
edit posts `WM_CLOSE` to its parent — documented behaviour of the EDIT
control — and the window obeyed, taking whatever was typed with it. Votraxxion's
fix for Tab (the edit stands aside for that one key in `WM_GETDLGCODE`) did
not cover it, because Votraxxion closes on Escape anyway. Here Escape means
"stop speaking", so both boxes now stand aside for Escape as well, and
`IsDialogMessage` turns it into the Stop command. The check is what found it;
it now tests both keys on both boxes.

### What turned out to be wrong, on the way

Two first attempts at this check did not measure what they claimed, and one
did harm:

- **The first version pressed Tab with `SendKeys`** after trying to bring the
  window to the front. The window did not come to the front, and forty Tabs
  and an Escape went to the window that was — a browser, on the development
  machine, while its owner was using it. Nothing depends on focus now; the
  script's header says so and says why.
- **The second version asked .NET's UI Automation client for names**, and got
  every control back as a `Pane` named by its own contents — "120", "0", "5"
  — which read as missing labels. They were not: without Windows'
  client-side proxies loaded, that API does not see Win32 controls the way a
  screen reader does. Asking MSAA, the layer NVDA actually uses here, gave
  the names above.

---

## 20.4 Building

```
cmake -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release     (from a vcvars64 shell)
cmake --build build-msvc
build-msvc\klattsch_gui.exe
```

**MSVC and clang-cl only.** MinGW's resource compiler, `windres`, runs its
preprocessor with the include directories unquoted, and this repository lives
under a path with a space in it; it fails before reading the `.rc`. The GUI
ships from MSVC, so the MinGW and WSL legs build the engine and the tools and
leave the window out, and `tools/build-matrix.ps1`'s new `gui` stage runs on
the two toolchains that build it.

---

## 20.5 What it leaves out, on purpose

- **The frozen constants** (GENERATOR.md sections 1–4) — the decision of
  2026-09-25.
- **Presets** — the shared preset format is the add-on's concern as much as
  this window's, and is designed once, with both.
- **The bank editor** (section 5).
- **Syntax highlighting** of the phoneme source: upstream's `highlight.js` is
  a browser feature, and colour carries nothing to a screen reader. The
  compiler's warnings in the Messages box carry what highlighting would.

---

## 20.6 The part a machine cannot do

A person with NVDA running:

- the window from the keyboard alone, every control, in order;
- what NVDA says for a spin box as it changes, and for the Messages box;
- Speak, Stop and Escape mid-sentence, Convert, Save;
- and the listening this window exists for — the voice, the stress, the
  contour's stand-in sizes from [19-frontend-text.md](19-frontend-text.md)
  §19.5.

What that finds is the next change to this chapter.

---

## 20.7 Step log

| Date | What |
|---|---|
| 2026-09-25 | Decision: GUI before the add-on, first version on the engine as it is, text input first. |
| 2026-09-25 | `klattsch_gui.cpp`, CMake target (MSVC, clang-cl), static CRT; `kl_text_dump --base`. |
| 2026-09-25 | `verify-gui.mjs`: 24 of 24 byte-identical. `gui-mutations.py`: 10 of 10. |
| 2026-09-25 | Accessibility check: the SendKeys attempt (keystrokes to the wrong window), the UIA attempt (wrong API), then MSAA. Found Escape closing the window; fixed. |
| 2026-09-25 | ctest 14/14 on MSVC; `gui-render` on clang-cl. |
| 2026-09-25 | First listening (the author, with NVDA): good, the comma pause a bit short. Comma pause box added; 26 of 26 byte-identical, 11 of 11 mutations, 21 stops all named. |
