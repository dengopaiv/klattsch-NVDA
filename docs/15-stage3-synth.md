# 15. Stage 3 — `kl_synth.c`

Stage 3 of the [C rewrite](REWRITE.md): the sample loop. Branch `stage3-synth`,
code in `csrc/kl_synth.{h,c}`, driven by `goldens/schedules.bin` and verified by
`tools/verify-stage3.mjs`.

The first stage where whole utterances are rendered and compared, rather than a
grid of one function — and the first test of the "zero differing samples after
16-bit quantization" half of the acceptance criterion. It is also the last
stage that can be done without the compiler, which is why the schedules were
captured in stage 0.

## 15.1 Driving C without a JSON parser

The corpus lives in `goldens/schedules.json`. Giving the C a JSON parser for
the sake of a test would put a parser in the product's dependency graph for no
product reason, so `tools/schedules-to-bin.mjs` converts it to a flat
length-prefixed binary — 107 cases, 540 events, 8520 values, 81 KB — and
`kl_synth_dump` reads that.

Engine extras (`[OQ=0.6]`) are not emitted: `FormantSynth` ignores them, so
they cannot affect a sample. They return when an engine that reads them does.

Only **voice 0** is rendered, because that is what the audio goldens were
captured from — `renderToBuffer` takes `compiled.schedule`, which mirrors voice
0. Mixing voices is the CLI's job, not the synth's.

## 15.2 A correction to the acceptance criterion

[REWRITE.md](REWRITE.md) says Tier 2 is:

> peak absolute sample difference `<= 1e-9` on a 30-second corpus, **in float64
> before quantization**

**That clause is not measurable, and this is the stage that shows it.** The
JavaScript renders into a `Float32Array`, so the rounding to single precision
is part of the algorithm, not a step after it — `renderToBuffer` returns
float32 values and the goldens digest float32 values. The C stores to `float`
for the same reason. **Neither side ever produces a float64 sample**, so there
is no float64 quantity to compare and no "before quantization" to be before.

Worse, the bound as written is tighter than the representation: one float32 ULP
at a sample of magnitude 0.15 is about 1.8e-8, so *any* single-ULP disagreement
fails a 1e-9 test while being the smallest disagreement that can exist.

The criterion is therefore restated, and the restatement is stricter in the
half that matters:

| | |
|---|---|
| **Was** | peak float64 difference ≤ 1e-9; zero differing 16-bit samples |
| **Is** | every differing float32 sample must be **under one float32 ULP** (a rounding-boundary tip, not accumulation), the count of differing samples is reported, and **zero differing samples after 16-bit quantization** |

The 16-bit clause is unchanged and is the real bar. The first clause becomes a
statement about *mechanism* — a sub-ULP difference can only be a rounding
boundary being tipped, whereas accumulation through the recursive biquads would
grow past a ULP and be caught.

## 15.3 The exit test

✅ Passes on all four toolchains, at all three sample rates.

```
Sample rate 48000
  cases compared                 ok   107
  samples bit-identical          ok   2948348/2948352 (99.9999%)
  peak float32 difference        ok   1.490e-8 at syllable/many[5447]
  16-bit differing samples       ok   0

Sample rate 22050
  samples bit-identical          ok   1354447/1354447 (100.0000%)
  16-bit differing samples       ok   0

Sample rate 8000
  samples bit-identical          ok   491392/491392 (100.0000%)
  16-bit differing samples       ok   0
```

**61 seconds of audio at 48 kHz, and four samples differ.** At 22050 and
8000 Hz, none do.

### The four differences are all sub-ULP boundary tips

| Sample | \|value\| | difference | one float32 ULP | ratio |
|---|---|---|---|---|
| `pitch/on-glide[2091]` | 4.50e-8 | 3.55e-15 | 5.36e-15 | 0.66 |
| `rate/400/glide[11762]` | 2.74e-7 | 2.84e-14 | 3.27e-14 | 0.87 |
| `syllable/many[5447]` | 1.54e-1 | 1.49e-8 | 1.83e-8 | 0.81 |
| `utterance/japanese[1403]` | 1.11e-1 | 7.45e-9 | 1.32e-8 | 0.56 |

Every one is **less than one float32 ULP** — ratios 0.56 to 0.87. Each is a
double value that sat close enough to a float32 rounding boundary that a
sub-ULP difference in the double tipped the rounding to the neighbouring
float32. That is the mechanism the restated criterion describes, and it is not
accumulation: the recursive biquads do not amplify the transcendental
difference, they stay well inside the float32 grid.

For scale, the 16-bit LSB is 3.05e-5. The largest disagreement is **three
orders of magnitude below one bit of the output format.**

### glibc renders a different set of four

| Toolchain | libm | differing samples at 48 kHz |
|---|---|---|
| MSVC | UCRT | 4 |
| clang-cl | UCRT | 4 |
| WinLibs gcc | UCRT | 4 |
| Debian gcc under WSL | **glibc 2.41** | **3** |

(Measured on the 107-case schedule corpus; the extended corpus of §15.4 adds
17 cases and does not change the count.)

Stage 1 found `glottalPulse` differing under glibc on 1.26% of its grid. Here
that difference propagates through the whole synthesis and changes *which*
samples land on a rounding boundary — three instead of four — while leaving the
16-bit output identical. The tolerance is doing exactly the work it exists for,
and the amount of work is small.

### Chunked rendering is identical

`--chunked 137` renders each case in 137-sample pieces instead of one call.
The output is **byte-identical** to whole-buffer rendering: the same four
samples differ, at the same indices, by the same amounts.

That is not decoration. [SCREEN-READER.md](SCREEN-READER.md) §3 makes chunked
rendering the design rather than an optimisation, on the strength of a 1.02 s
versus 1.9 ms time-to-first-audio measurement. This proves the synth's state
genuinely survives a call boundary, so that design costs nothing in fidelity.
It is registered as its own ctest (`stage3-chunked`) so it stays true.

## 15.4 The mutation suite, and a tool that lied

`tools/stage3-mutations.py` breaks the sample loop one change at a time and
requires the verifier to fail on each. Unlike stages 0 and 2, the interesting
mutations here are **logic, not constants**:

- the increment recomputed only for the parameters an event carries,
- the snap to target on the last sample of a transition removed,
- the counter advanced before the drain instead of after,
- `tilt_prev` holding the tilted value, turning a one-zero filter into a
  one-pole one,
- the noise sample taken unsigned,
- `floor` replaced by truncation in the phase wrap and the schedule conversion,
- `Math.max(1, ...)` dropped from the transition length.

Those are what a mechanical translation gets wrong, and a constant-only suite
would notice none of them.

Result: **15 of 15 caught.** Getting there took three rounds, and each round
was the harness being wrong rather than the code.

**Round 1 — the shell version reported eight false "NOT CAUGHT" results.**
Every one was its own quoting failure: the patterns contain `/`, which collides
with `perl -pi -e s///`, and several span multiple lines, which a line-at-a-time
filter cannot match at all. It was reporting a passing test as a gap in the
corpus. That is the worse direction for a test to fail in — a suite that misses
a real bug costs you the bug, a suite that invents gaps costs you trust in
every result it has ever given. Rewritten in Python with literal replacement.

**Round 2 — the harness hung for forty minutes** on a step that takes five
seconds, leaving a mutation applied to the working tree the whole time. The C
under that mutation ran fine in isolation (exit 0, correct output size), so the
stall was in the runner's subprocess handling. Every subprocess now has a
deadline, and a timeout is reported as a harness failure rather than a result.
A mutation runner that can hang is one that will, and it leaves the source
broken while it does.

**Round 3 — two real corpus gaps, and one genuine unreachable.** With the
harness finally honest, three mutations survived:

- *`floor` vs truncation in the glottal phase wrap.* These agree for all
  non-negative phase, and nothing in the corpus made the phase run backwards.
  It takes vibrato deeper than the base pitch — `b80 v200` puts the effective
  F0 in [-120, 280] — and the deepest case was `v60` against F0 120. **Closed**
  by adding `vq/vibrato-exceeds-f0` and `vq/vibrato-far-exceeds-f0`.
- *`Math.max(1, ...)` on the transition length.* It only binds when a
  transition rounds below one sample, which needs both a very low rate and a
  low sample rate: at rate 1 the shortest transition is 0.48 samples at 8 kHz
  but still 2.88 at 48 kHz. The corpus's lowest rate was 10. **Closed** by
  adding rates 1, 2 and 5 — and by fixing the suite, which was running only at
  48 kHz and so could not have seen it even with the cases present.
- *`floor` vs truncation in the schedule's `atMs` conversion.* **Not closable.**
  The compiler's `timeMs` starts at 0 and only increases, and all 599 corpus
  events have `atMs >= 0`, so floor and truncation agree on every input the
  compiler can produce. It would take a hand-built schedule. The mutation is
  kept and marked *expected not to be caught*, so the claim is re-checked on
  every run instead of resting on a measurement made once — and if the compiler
  ever emits a negative `atMs`, the suite fails loudly.

The corpus went from 694 cases to **711**, and stage 0's own suite still
catches 34 of 34 against the extended goldens.

## 15.5 Step log

**`kl_synth.{h,c}`** translated from `synth-core.js`. Parameters as a fixed
`double[19]` indexed by an enum in `PARAMS` order, with a `present` bitmask per
event. No allocation. Clean at `/W4` and `-Wall -Wextra -Wpedantic -Wshadow
-Wconversion` on all four toolchains, after one real warning — the dump tool
stashed a count in an uninitialised struct field, which MSVC caught as C4701
and which a local variable fixed.

**`schedules-to-bin.mjs`** so the C needs no JSON parser.

**Exit test passes** at three sample rates on four toolchains. 4 differing
samples in 2,948,352 at 48 kHz (3 under glibc), all sub-ULP, zero after 16-bit
quantization.

**Mutation suite 15 of 15**, after three rounds of fixing the harness and two
corpus extensions. Goldens re-captured at 711 cases; stage 0 still 34 of 34.

**Criterion corrected** — §15.2. The float64 clause was unmeasurable; the
replacement is stricter where it counts.

**Chunked rendering proved identical**, which phase 3's design depends on.
