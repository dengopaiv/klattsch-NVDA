# The GUI sample generator

A desktop program for driving the C engine with **every parameter it has
exposed** — including the ones upstream freezes as constants — auditioning the
result, and saving presets and WAV files.

Its job is not to be a nicer front end for the web app. Its job is to make the
frozen constants in [ARCHITECTURE.md](ARCHITECTURE.md) reachable, so the voice
can actually be designed rather than accepted.

## The parameter surface, complete

This is the enumeration the requirement "all the possible parameters exposed"
resolves to. Parameters marked **frozen** have no directive in the JavaScript
engine today and become real parameters in the C engine. Parameters marked
**new** do not exist yet and arrive with step 7 of [REWRITE.md](REWRITE.md);
each defaults to the value that reproduces the original.

### 1. Source

| Parameter | Range | Default | Status |
|---|---|---|---|
| `F0` / base pitch | 40–600 Hz, or a note name | 120 | live |
| `voicing` | 0–1 | per phoneme | live |
| `effort` (pulse shape) | 0–1 | 0.5 | live |
| `aspiration` | 0–1 | 0 | live |
| `Tp` at effort 0 / at effort 1 | 0.05–0.9 | 0.5 / 0.3 | **frozen** |
| `Tn` at effort 0 / at effort 1 | 0.01–0.5 | 0.25 / 0.08 | **frozen** |
| pulse normalization | 0.01–1 | 0.1 | **frozen** |
| voiced gain vs. aspiration | 0–1 | 0.85 | **frozen** |
| unvoiced noise level | 0–1 | 0.35 | **frozen** |
| aspiration noise level | 0–1 | 0.5 | **frozen** |
| LFSR seed | any u32 | `0xACE1ACE1` | **frozen** |
| jitter (cycle-to-cycle F0) | 0–1 | 0 | **new** |
| shimmer (cycle-to-cycle amplitude) | 0–1 | 0 | **new** |
| flutter (slow multi-LFO F0 drift) | 0–1 | 0 | **new** |
| diplophonia | 0–1 | 0 | **new** |

### 2. Resonators

| Parameter | Range | Default | Status |
|---|---|---|---|
| `F1`–`F3` | 40 – 0.45·sr | per phoneme | live |
| `BW1`–`BW3` | ≥20 Hz | per phoneme | live |
| `A1`–`A3` | 0–2 | per phoneme | live |
| `scale` (all formants) | 0.5–2 | 1.0 | live |
| `F4`–`F6`, `BW4`–`BW6`, `A4`–`A6` | | off | **new** |
| formant count | 3–6 | 3 | **new**, build dimension |
| `FNP` / `FNZ` nasal pole and zero | | off | **new** |
| cascade path on/off, cascade gain | | off | **new** |
| biquad clamp floor / ceiling | | 40 Hz / 0.45·sr, 20 Hz BW | **frozen** |

### 3. Output stage

| Parameter | Range | Default | Status |
|---|---|---|---|
| `gain` | 0–10 | 3.5 | live |
| `tilt` | −0.95–0.95 | 0 | live |
| `tremoloDepth` / `tremoloRate` | 0–1 / Hz | 0 / 5 | live |
| `vibratoDepth` / `vibratoRate` | Hz / Hz | 0 / 5 | live |
| soft-clip threshold | 0.1–1 | 0.85 | **frozen** |
| soft-clip knee shape | | `e/(e+1)` | **frozen** |
| peak normalize target | 0–1, or off | 0.95 | live (encoder option) |
| sample rate | 8000–96000 | 48000 | live |

### 4. Timing and prosody — the compiler's constants

Every row here is frozen in the JavaScript.

| Parameter | Default | Status |
|---|---|---|
| `rate` (ms per phoneme) | 110 | live |
| `stressDurationFactor` | 1.5 | **frozen** |
| `stressF0Lift` | 8 Hz | **frozen** |
| `defaultTransitionMs` | 35 | **frozen** |
| `stopBurstMs` | 25 | **frozen** |
| stop pre-silence cap | 20 ms | **frozen** |
| stop burst fraction of slot | 0.3 | **frozen** |
| stop silence fraction for the cap | 0.4 | **frozen** |
| stop burst transition cap / fraction | 5 ms / 0.2 | **frozen** |
| glide onset / glide / offset split | 0.25 / 0.50 / 0.25 | **frozen** |
| glide onset transition cap | 20 ms | **frozen** |
| pitch-move onset fraction / cap | 0.25 / 25 ms | **frozen** |
| pitch-move ramp fraction | 0.6 | **frozen** |
| steady transition fraction | 0.4 | **frozen** |
| `sentenceFinalHoldMs` | 0 | **frozen** |
| `fadeOutMs` | 100 | **frozen** |
| `trailOffMs` | 150 | **frozen** |
| pause `,` / `;` / `.` | 100 / 200 / 300 ms | **frozen** |
| default pause transition | 30 ms | **frozen** |
| per-phoneme intrinsic duration | — | **new**, bank schema v2 |

### 5. Phoneme editing

Direct editing of the active bank, per phoneme: `voicing`, `F1`–`F3`,
`BW1`–`BW3`, `A1`–`A3`, `isStop`, `glideTo`, and the new `durationMs`. Edits
are held in an overlay bank that `extends` the base one, so saving produces a
valid bank JSON that the engine, the CLI and the add-on all load unchanged.
This is the format the bank registry already supports — an overlay is not a new
mechanism, it is the `extends` field used as designed.

### 6. Text and source

The phoneme string itself, with the syntax highlighting upstream already ships
in `src/highlight.js`; the text front end from
[NVDA-ADDON.md](NVDA-ADDON.md) with its intermediate ARPABET visible and
editable; the per-voice sections of `[voice=N]`.

## What it is built in

Native Win32, C or C++, the same shape as `votraxxion/gui-native/` — one x64
`.exe`, no runtime to install, and it links the same static engine the add-on
does. 64-bit only, like everything else here. The GUI is the one place C++
earns its keep, and being a separate binary it can use it freely without
touching the C library.

**Accessibility is a build requirement, not a feature.** A tool for designing
screen-reader voices that a screen-reader user cannot operate is not finished.
Standard Win32 controls with correct labels, tab order and keyboard access to
every parameter; no owner-drawn slider that MSAA cannot see. `votraxxion`'s GUI
is the reference for what passes.

## Layout

Parameters are grouped by the sections above, which is also the order of the
signal path, because that is what makes an unfamiliar parameter findable. Each
one shows its value, its default, and whether it is currently overridden —
"what have I changed from stock" is the question this tool has to answer at a
glance, given how many parameters there are.

Three things always present:

- **Speak.** Renders and plays the current source with the current parameters.
- **Save WAV.** With the source string in the `ICMT` chunk, as the JS encoder
  already does, so a rendered file says how to regenerate itself.
- **Save preset.** The bundle described below.

## Presets

One JSON file. The same format the NVDA add-on reads as a voice:

```json
{
  "schemaVersion": 1,
  "name": "...",
  "displayName": "...",
  "engine":  { "...": "everything in sections 1-4" },
  "bank":    "klatt1980-en",
  "overlay": { "...": "an optional bank that extends the named one" }
}
```

Only overridden values are written; anything absent takes the engine default.
That keeps presets diffable, keeps them small, and means a preset written today
still loads after a new parameter is added.

A preset saved here drops into the add-on's preset directory and appears in
NVDA's voice list with no conversion step. That round trip is the reason to
build both programs against one engine.

## Order

- [ ] **1.** Engine steps 0–6 in [REWRITE.md](REWRITE.md) green, and the
      frozen constants of sections 1–4 promoted to real parameters with
      defaults proven to reproduce the goldens exactly.
- [ ] **2.** Preset format and loader, in C, shared by the CLI and the add-on.
      Exercised from the CLI first: `klattsch --preset x.json "HH AH L OW"`.
- [ ] **3.** The GUI: sections 1–4, speak, save WAV, save preset.
- [ ] **4.** Section 5, the bank editor and overlay saving.
- [ ] **5.** Section 6, text front end and ARPABET view.
- [ ] **6.** Accessibility pass with NVDA actually running.
