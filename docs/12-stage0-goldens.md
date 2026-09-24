# 12. Stage 0 — the goldens

Stage 0 of the [C rewrite](REWRITE.md): capture the JavaScript engine's
behaviour as a golden corpus, before a line of C exists, and prove the corpus
can fail. Branch `stage0-goldens`, code in `tools/goldens.mjs` and
`tools/golden-mutations.sh`, output in `goldens/`.

This is the first test this codebase has ever had. There was no existing corpus
to reuse or cross-check against, so the corpus is entirely our own work and its
quality is the only thing standing between the port and a silent behavioural
change.

## 12.1 What it captures

694 cases across 19 groups, plus the primitives tested directly.

> **The corpus has grown since.** Later stages found paths it did not reach and
> closed them: stage 3 added the `vq` group and took it to 711
> ([15-stage3-synth.md](15-stage3-synth.md) §15.4), stage 4 took it to **714**
> ([16-stage4-token.md](16-stage4-token.md) §16.6). The numbers in this chapter
> are what was captured at stage 0 and are left as they were; `goldens/manifest.json`
> always holds the current count. Every extension was driven by a mutation that
> survived, never by a stage needing to pass.

| Group | Cases | What it answers |
|---|---:|---|
| `phoneme`, `phoneme-frame` | 258 | every phoneme in all three banks, alone and framed |
| `note` | 233 | `A-1` to `B9`, natural, sharp and flat |
| `rate` | 50 | either side of every `min()` cap in `renderPhoneme` |
| `directive` | 43 | every letter, in absolute / `=` / relative / bare-reset form |
| `voice-quality` | 18 | aspiration, tremolo, vibrato, tilt, effort, unvoiced |
| `utterance` | 13 | whole utterances; the Tier 2 audio corpus |
| `syllable` | 10 | including nested, unmatched, unclosed |
| `pitch` | 9 | sticky and transient, on stops and glides |
| `comment`, `normalize` | 16 | every comment position; homoglyphs and zero-width |
| `voice` | 7 | 1, 2 and 5 sections, empty sections, no carry-across |
| `extras` | 7 | set, override, clear, from opts, `F4` scaling |
| `bank` | 4 | switch, reset-to-opts, unknown |
| `engine`, `pause`, `stress`, `unknown`, `edge` | 26 | markers, pauses, stress marks, warnings, degenerate input |

Audio is rendered for six of those groups at **48000, 22050 and 8000 Hz**. The
low rate is not padding: the biquad clamps to `0.45 * sr`, so 8 kHz is the only
rate at which the upper clamp on F3 ever binds.

The whole corpus is 900 KB of JSON across six files, and captures in 3.4 s.

## 12.2 How a value is compared

Digests are SHA-256 over a byte encoding a C implementation can reproduce
exactly: **doubles go in as eight little-endian IEEE-754 bytes, never as
text**, so the digest is insensitive to how either language formats a float.
The encoding is defined once, in `Digest` at the top of `tools/goldens.mjs`.

A schedule digest covers, per event: `atMs`, `transitionMs`, then the 19
parameters of `PARAMS` in fixed order each with a present/absent flag, then the
engine-specific extras sorted by key.

**One documented exclusion.** `scaled()` in the compiler spreads the whole
phoneme object, so `isStop` and `glideTo` ride into every schedule target.
`FormantSynth` ignores them because it iterates `PARAMS`; the C struct will not
have them at all. They are excluded from the digest, in one place
(`NON_PARAM_TARGET_KEYS`), rather than as a tolerance spread through the
comparison. `voicing` is *in* `PARAMS` and is digested — the code says so
explicitly, because it is the one field where "is this a phoneme flag or a
parameter?" has a non-obvious answer.

Audio yields two digests per case: float64 for the Tier 2 tolerance comparison,
and int16 after quantization for the "zero differing samples" requirement.

## 12.3 The exit test

Three parts, all three required.

**1. Determinism.** `node tools/goldens.mjs --check` re-captures and diffs.
Nothing in the output may record a timestamp, a path or a duration, or the
goldens would differ from themselves. ✅ passes.

**2. Coverage.** Every item in the corpus list of [REWRITE.md](REWRITE.md) has
a group above. ✅

**3. A deliberately broken engine is caught.** `tools/golden-mutations.sh`
mutates one engine constant at a time — 33 of them, across `dsp.js`,
`synth-core.js`, `sequencer.js` and the banks — and requires `--check` to fail
on each, reverting with git between mutations. ✅ passes, after the corpus was
fixed twice; see below.

A golden corpus that cannot fail is worse than none, because the one time it
matters nobody looks. Part 3 is the whole point of the stage.

## 12.4 What the mutation run found

It failed on the first run, which is the reason it exists. Four mutations
slipped through.

**Three were real holes in the corpus.** `voicedGain 0.85`, `aspiration noise
0.5` and the tremolo modulator shape could all be changed with no golden
moving. The cause was the same for all three: **not one rendered case set
`aspiration` or `tremoloDepth` to anything but zero.** Those constants are
multiplied by the parameter, so at zero they multiply out of the expression
entirely and become unobservable. The `directive/h/*` and `directive/m/*` cases
did set them, but `directive` is not an audio group, so only the schedule was
compared — and the schedule was correct. The bug was invisible precisely
because the compiler half was right.

Fixed by adding the `voice-quality` group: 18 rendered cases that set
aspiration, tremolo, vibrato, tilt and effort across their ranges, including
the endpoints and the unvoiced path. All three mutations are now caught.

**The fourth was a wrong expectation, not a hole.** Changing `"F1": 310` in
`klatt1980-en.json` moved no golden. That is correct: **the engine imports
`bundled.js`, not the JSON.** The JSON is the generator's source, and only
`bundled.js` can change what the engine does — a mutation there is caught. The
JSON is `build-banks.js --check`'s job, which is wired to CI for exactly this
reason.

Rather than delete the mutation, the script now asserts the division of labour
in both directions: a JSON edit must be **invisible to the goldens** and
**caught by `build-banks --check`**. Two guards, two jobs, and a test that says
which is which. The wrong expectation was more informative than the passing
test would have been.

## 12.5 What the goldens do not cover, and why

- **`pronounce.js` and `kana.js`.** `pronounce.js` needs an optional
  dependency that is not installed, and neither is part of the engine the C
  port replaces. Out of scope for stage 0; they return when the front end does.
- **`highlight.js`.** Editor sugar, not engine.
- **The AudioWorklet wrapper.** 59 lines of message plumbing around
  `FormantSynth`, and no browser is available to run it. Its behaviour is
  `queueSchedule` and `setTarget`, both of which are covered through the
  engine. The `startTime` anchoring arithmetic is not, and is noted here as a
  gap for whoever ports the worklet.
- **Long-run numeric drift.** Every case is seconds long. If a difference
  accumulates only over minutes of continuous rendering, this corpus will not
  see it. The chunked-rendering work of phase 3 is where that becomes testable,
  since it renders continuously across chunk boundaries.

## 12.6 Step log

**Corpus written, 694 cases.** Built from the list in
[REWRITE.md](REWRITE.md), one group per way a rewrite can break. Captures in
3.4 s to 900 KB.

**Determinism confirmed.** `--check` clean on a re-run. No timestamps or paths
in the output.

**Mutation run 1: 29 caught, 4 missed.** Three corpus holes (aspiration and
tremolo never non-zero in a rendered case), one wrong expectation (the JSON is
not what the engine reads).

**Corpus extended to 694 with the `voice-quality` group**, and the bank
mutation rewritten as a two-directional assertion about which guard owns the
JSON.

**Mutation run 2: all caught.** Recorded in the commit.
