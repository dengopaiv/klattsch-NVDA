# Stage 6 — `kl_wav.c`, the mix, and the CLI

Translated from `src/engine/wav.js` and from `bin/klattsch.mjs`. klattsch is
Tony Gies's work; this is a translation of part of it and carries the same MIT
notice — including the `ISFT` field that says so in every file the engine
produces.

This is the first stage whose output is a **program**. Stages 1 to 5 built a
library and proved it matched, one layer at a time; this one puts the layers
together, writes a file, and compares that file with the one the JavaScript
writes.

**Exit test:** the whole corpus rendered to WAV at all three sample rates,
every file byte-identical to the JavaScript CLI's, on MSVC, clang-cl, WinLibs
gcc and WSL Debian gcc. ✅ Passes — 2,187 files, 41.1 MB, no tolerance
anywhere.

---

## 18.1 What stage 6 is

Four files, three of them library and one of them the program.

| | |
|---|---|
| `csrc/kl_wav.c` | `src/engine/wav.js` — the RIFF header, the `LIST INFO` chunk, peak normalization, 16-bit samples |
| `csrc/kl_render.c` | the six lines in the middle of `bin/klattsch.mjs` that render each voice section and sum them |
| `csrc/kl_fmt.c` | `Number.prototype.toFixed`, for the one line the program prints |
| `bin/klattsch_cli.c` | `bin/klattsch.mjs` — argument for argument |

`kl_render.c` exists because upstream's mix has no home of its own: it is six
lines inside a script. They are not incidental — they decide the rounding of
every multi-voice utterance — so they became a translation unit with a name
rather than being inlined twice.

`kl_fmt.c` exists for one function, and §18.3 is why.

---

## 18.2 The measurement that had to come first

[REWRITE.md](REWRITE.md) sized this stage **small, low risk**. The size was
right. The risk was not, and the reason is one line of `wav.js`:

```js
if (peak > 0) gain = peakNormalize / peak;
```

**Every byte of the file depends on the single loudest sample of the mix.**
Stage 3 had already measured that C and JavaScript disagree on 4 of 2,948,352
float32 samples at 48 kHz — sub-ULP rounding tips, three orders of magnitude
below one bit of the output — and concluded that the 16-bit output was
identical. That conclusion was drawn at gain 1. With normalization it does not
carry: if one of those four samples is ever the peak, the gain moves by a
relative 1e-7, every scaled sample shifts by up to 3e-3 LSB, and something like
a thousand samples flip.

So before a line of `kl_wav.c` was written, the existing stage-3 dumps were put
through the CLI's own pipeline — mix, normalize, quantize — and compared.

| toolchain | rate | float32 samples differing | gains differing | 16-bit samples differing |
|---|---|---|---|---|
| MSVC (UCRT) | 48000 | 4 | **0** | **0** |
| MSVC | 22050 | 0 | 0 | 0 |
| MSVC | 8000 | 0 | 0 | 0 |
| Debian gcc (glibc 2.41) | 48000 | 3, a different set | **0** | **0** |

None of the differing samples is the peak of its buffer on either libm, so the
gain is bit-identical, and the two disagreements large enough to matter sit far
from a quantization boundary:

| sample | disagreement, in LSB | distance to the nearest boundary | margin |
|---|---|---|---|
| `syllable/many[5447]` | 5.39e-4 | 0.324 | 601× |
| `utterance/japanese[1403]` | 2.67e-4 | 0.0975 | **365×** |

That was enough to say the stage's bar could be *byte-identical* rather than
*identical within a tolerance*. It is the measurement that set the exit test,
and it was worth ten minutes before it was worth ten hours.

It is not the whole answer, and was not claimed to be. Stage 3's dumps are
voice 0 only — `kl_synth_dump` says so, and says that mixing voices is the
CLI's job — so the probe covered the single-voice path on two libms and left
the ten multi-voice cases and the 603 cases C had never rendered to the exit
test itself. Both came back clean.

### How much luck is in that

The verifier now measures the same thing over the whole corpus, and the number
is not comfortable:

```
  closest 16-bit rounding boundary           5.84e-9 LSB at note/Gs7[853]
  samples a 1-ULP slip could flip            11782 of 13,192,272
```

**0.089% of samples at 48 kHz sit within one float32 ULP of a rounding
boundary.** Stage 3 finds four samples where two libms disagree by one ULP. If
those four fell at random, the chance that none of them lands on a boundary is
about 99.6% — so byte-identity holds, and holds with a margin of 365× at the
tightest point, but it holds *per toolchain* rather than by construction.

That is not a defect to fix; it is a property of a format that quantizes to 16
bits and a pipeline that normalizes. What it changes is how the result must be
stated: the exit test is a measurement repeated on four toolchains, not a
theorem. The verifier prints both numbers on every run so that a future change
that walks the margin toward zero is visible before it flips a byte.

---

## 18.3 Three rounding rules, none of them a C library function

`wav.js` rounds samples with `Math.round` and the CLI prints numbers with
`toFixed`. Neither is what a translator reaches for first.

**`Math.round` is not `round()`.** C breaks ties away from zero; JavaScript
breaks them toward +∞. `Math.round(-1.5)` is `-1`; `round(-1.5)` is `-2`.

**`Math.round` is not `floor(x + 0.5)` either.** At `x = 0.49999999999999994`
the addition rounds up to exactly `1.0`, so `floor(x + 0.5)` is 1 where
`Math.round` is 0. The specification says so explicitly, and V8 implements it.

`kl_wav_round` therefore computes the fraction and tests it:

```c
r = floor(x);
d = x - r;                       /* exact for every finite x */
y = (d >= 0.5) ? r + 1.0 : r;
if (y == 0.0) y = signbit(x) ? -0.0 : 0.0;
```

`x - floor(x)` is exact: for |x| ≥ 1 the two are within a factor of two of each
other, so Sterbenz applies, and for |x| < 1 the floor is 0 or −1 and the
subtraction is trivially exact. That is what lets the tie be tested with `==`
and no tolerance. The last line is `Math.round` returning `-0` for every `x` in
[−0.5, −0]; it makes no difference to a sample, and it makes a difference to a
sweep that compares with `Object.is`.

**`toFixed` is not `printf`.** `printf("%.*f")` rounds the exact binary value to
nearest with ties to even. `toFixed` strips the sign and rounds the magnitude
with ties away from zero. They disagree only on an exact tie — and an exact tie
is exactly the set `x = odd / 2^(d+1)`:

> a tie means `x = (2k+1) / (2·10^d)`; a double is a dyadic rational, so the
> factor `5^d` must divide `2k+1`, and what is left is `odd / 2^(d+1)`.

`ldexp` by `d+1` is exact, so `kl_to_fixed` tests for a tie without parsing a
printed string.

Both ties are reachable, which is why `kl_fmt.c` is in the library rather than
a paragraph in a commit message. A tie is only a *disagreement* when the lower
neighbour is even, since that is when ties-to-even rounds down where `toFixed`
rounds up — and measured over the 460 corpus cases the CLI can actually be
driven with:

* **the byte count disagrees on three of them.** `syllable/unclosed`,
  `voice/leading-marker` and `extras/names-a-parameter` all render to exactly
  25,088 bytes, which is 24.5 KB. `printf("%.0f")` prints 24; the JavaScript
  prints 25. The first is in the end-to-end set, so this tie needs nothing
  built.
* **the seconds figure never ties at all** — not one of the 460 makes
  `totalMs / 1000` an exact tie at two decimals. That one is *constructed*:
  `[rate=175] AA .` compiles to exactly 625 ms, and `(0.625).toFixed(2)` is
  "0.63" where `printf("%.2f")` gives "0.62".

---

## 18.4 The `LIST INFO` chunk, and the attribution in it

The chunk goes **after** the data chunk, holds `ISFT` before `ICMT`, and pads
each sub-chunk to an even length with a zero byte that is counted in the LIST
payload size but *not* in the sub-chunk's own size field. Three separate ways
to get it subtly wrong, and a player that ignores the chunk will not tell you
about any of them.

`ISFT` is `klattsch · https://tgies.github.io/klattsch`, from
`bin/klattsch.mjs:37`. It is upstream's credit, and it travels in every file
this engine writes. Three things protect it:

1. it is a `#define` in `csrc/kl_wav.h`, not a string typed in two places;
2. it is spelled with a hex escape — `"klattsch \xC2\xB7 https://…"` — so that
   no compiler's source-charset default can alter it. MSVC without `/utf-8`
   reads a `.c` file in the system ANSI codepage, and this is the one string
   in the output that must survive a toolchain change intact;
3. the verifier extracts the string from `bin/klattsch.mjs` and compares it
   with the `#define`, so the two cannot drift apart, and separately checks
   that `bin/klattsch_cli.c` actually uses it.

An empty string is falsy in JavaScript, so a present-but-empty `software` field
is an *absent* one and the file gets `ICMT` without `ISFT`. That is a contract,
not an accident, and `goldens/wav.json` now has an `emptySoftware` case that
pins it.

---

## 18.5 The mix

```js
for (const v of voices) {
  if (!v.schedule.length) continue;
  const vb = renderToBuffer({ sampleRate, schedule: v.schedule, totalMs: v.totalMs });
  const n = Math.min(buf.length, vb.length);
  for (let i = 0; i < n; i++) buf[i] += vb[i];
}
```

Three things in six lines:

* **the sum is in float32.** `buf` is a `Float32Array`, so the rounding to
  single precision happens once per voice per sample. A double accumulator
  would be more accurate and would not be the same file.
* **each voice is as long as its own `totalMs`**, while the mix is as long as
  the longest section. `voice/leading-marker` has a voice 0 of 150 ms in a
  260 ms utterance, so `Math.min` really does pick the shorter of the two —
  though see below for what that turns out to be worth.
* **voice order is observable**, because float addition is not associative.
  `voice/five` sums five sections.

Ten of the 729 cases have more than one voice, and none of them was exercised
by any earlier stage: the golden harness renders `compiled.schedule`, which is
voice 0 alone. The mix is new surface in stage 6, and five cases mix past full
scale before normalization (`voice/five` peaks at 2.60).

### Rendering a voice too long is not observable

The mutation that renders every section to the length of the whole utterance
instead of its own **was not caught**, and the reason is worth keeping: every
one of the corpus's 744 voice sections ends with `A1 = A2 = A3 = 0`, because
every section closes with a fade-out. Past its own `totalMs` a section
therefore produces *exact* zeros — 5,280 to 15,840 of them in the five cases
that have a short section, not one of them non-zero — and adding zero to a
float changes nothing.

So `Math.min` is load-bearing for memory safety and not for the output. The
mutation is kept with `expect_caught=False` and the measurement beside it, so
that if a section ever ends with a non-zero amplitude the line turns into a
failure instead of staying quietly true.

### Two branches the corpus cannot reach

* **`if (!v.schedule.length) continue`.** Measured over all 729 cases and 744
  voice sections: **zero** have an empty schedule, because every section ends
  with a trail-off. Kept, because the JavaScript keeps it, and the mutation
  that removes it is marked as expected-not-caught so the claim is re-checked
  on every run.
* **the clamp, `if (s > 1) s = 1`.** With `peakNormalize = 0.95` the loudest
  scaled sample is 0.95 by construction, so the clamp cannot bind — not even
  for the five cases that mix past 1.0. The only way in is normalization off,
  which the CLI never does, so `goldens/wav.json` gained a `clipped` case that
  encodes a buffer scaled by three with `peakNormalize: 0`.

A third, `Math.ceil(0)` producing an empty buffer, is unreachable for the same
reason as the first: every utterance has a trail-off, so `totalMs` is never 0.
The `empty` golden encodes a zero-length buffer directly and pins the 44-byte
file, which also reaches the `if (peak > 0)` guard.

---

## 18.6 Windows hands you the wrong bytes

`kl_tokenize` takes UTF-8. `main()` on Windows is handed the command line in
the system ANSI codepage. The phoneme grammar is ASCII, so this would look
fine — except that the text also becomes the `ICMT` comment, and the corpus has
cases that are not ASCII: `normalize/cyrillic` is `АА`, `normalize/nfkc-fullwidth`
is `ＡＡ`.

So the CLI is `wmain` on Windows and converts with
`WideCharToMultiByte(CP_UTF8, …)`, and the output file is opened by converting
back to UTF-16 for `_wfopen`, because `fopen` would put a UTF-8 path through
the ANSI codepage and write the file under a different name. MinGW needs
`-municode` to link the Unicode entry point; without it the ANSI `main` is
linked and the arguments arrive mangled, so `CMakeLists.txt` adds it for that
toolchain only.

Both paths are verified end to end rather than argued for: two of the
end-to-end cases have non-ASCII *arguments*, and one writes to a non-ASCII
*filename* (`ハロー.wav`).

---

## 18.7 The exit test

`tools/verify-stage6.mjs`. Six sections, in the order a failure is easiest to
read in.

1. **The reference has not moved.** Ten fragments of `bin/klattsch.mjs`, each
   required to occur exactly once, plus the `ISFT` comparison. The mix is
   reproduced in the verifier rather than imported — there is nothing to
   import, upstream's CLI being a script — so this is what keeps the copy
   honest. If upstream's CLI is ever edited, this fails loudly instead of
   quietly verifying a stale pipeline.
2. **`kl_wav_round` against `Math.round`**, swept.
3. **`kl_to_fixed` against `toFixed`**, swept.
4. **The seven encoder goldens.**
5. **The corpus**, at 48000, 22050 and 8000 Hz: whole files, compared byte for
   byte, with the gain compared separately as an exact double because a gain
   that differs is a different file in every sample.
6. **The CLI itself**, end to end: both real programs run on the same text,
   and their files *and* their stderr lines compared.

### Why the corpus does not go through the CLI

`bin/klattsch.mjs` takes a phoneme string and nothing else. **269 of the 729
cases** carry a bank, an engine or seeded extras, and no argument can reach
them. Two ways out of that: give both CLIs matching options — which means
editing the frozen reference — or drive the pipeline directly and prove
separately that the CLI is that pipeline.

The second was chosen. `csrc/tools/kl_wav_dump.c` renders the corpus through
the same `kl_render` and `kl_wav` the CLI uses, and section 6 runs both real
programs on 12 invocations and compares the files they write. The exit-test
wording in REWRITE.md was corrected to match, because "the CLI renders the
whole corpus" was never something either CLI could do.

### Numbers

```
  the reference bin/klattsch.mjs
  the CLI's pipeline is unchanged            ok     10 fragments
  KL_WAV_SOFTWARE matches the JS             ok     "klattsch · https://tgies.github.io/klattsch"
  the CLI writes it                          ok     bin/klattsch_cli.c uses KL_WAV_SOFTWARE

  kl_wav_round against Math.round
  every value identical                      ok     166 values, 29 exact ties, 6 give -0

  kl_to_fixed against toFixed
  every value identical                      ok     309 values, 133 exact ties

  the encoder goldens
  every encoding matches the golden          ok     7/7

  the corpus at 48000 Hz
  every file byte-identical                  ok     729/729
  normalization gain exact                   ok     729/729
  bytes compared                             --     25.2 MB over 13,192,272 samples
  closest 16-bit rounding boundary           --     5.84e-9 LSB at note/Gs7[853]
  samples a 1-ULP slip could flip            --     11782 of 13,192,272

  the corpus at 22050 Hz
  every file byte-identical                  ok     729/729
  normalization gain exact                   ok     729/729
  bytes compared                             --     11.6 MB over 6,060,352 samples
  closest 16-bit rounding boundary           --     1.33e-7 LSB at phoneme/ja-hecko-2026/CH[2388]
  samples a 1-ULP slip could flip            --     4626 of 6,060,352

  the corpus at 8000 Hz
  every file byte-identical                  ok     729/729
  normalization gain exact                   ok     729/729
  bytes compared                             --     4.3 MB over 2,198,712 samples
  closest 16-bit rounding boundary           --     7.84e-9 LSB at utterance/pangram[3225]
  samples a 1-ULP slip could flip            --     1581 of 2,198,712

  bin/klattsch_cli.c against bin/klattsch.mjs
  the two programs write the same file       ok     12/12
  and print the same thing                   ok     12/12
```

**2,187 whole files. 41.1 MB. Every byte.** And no tolerance anywhere in stage
6 — the arithmetic that could need one is all upstream of here, in stage 3.

### What the goldens do and do not freeze

`goldens/wav.json` freezes the encoder: seven encodings of one buffer, checked
in. The corpus comparison is live JavaScript against the C, not against a
frozen digest, and that is a deliberate difference from stages 1 to 5.

Freezing 2,187 file digests would have meant rendering the whole corpus inside
`tools/goldens.mjs`, taking `goldens.mjs --check` from 4.7 s to about 20 s — a
test that runs on every build. What it would buy is protection against the
JavaScript reference changing under the comparison, and that is already bought:
`goldens-current` regenerates the schedules from the engine and compares them,
so an engine change fails there first. The WAV bytes are a pure function of the
schedule, the sample rate and the encoder, and the encoder is what `wav.json`
pins.

---

## 18.8 The mutation suite

`tools/stage6-mutations.py`, 44 mutations in seven groups. The verifier passed
on its first run at all three rates, exactly as stage 5's did, so this is again
the thing that decides whether that meant the translation was right or the
comparison was blind.

```
caught 44, missed 0
All mutations caught.
```

### First run: 42 caught, 2 missed — and neither was a gap in the corpus

That is new. In stages 3, 4 and 5 every miss was a hole in the corpus, closed
by adding cases. Here both misses were **faults in the suite**, and each took a
measurement to tell apart from a real gap.

**"An empty software string becomes a present one" was a no-op.** The mutation
changed `if (meta->software && meta->software[0])` to `if (meta->software)` —
and `strlen("")` is 0, so both spellings compute the same length and emit the
same file. There is no one-line edit that can introduce this bug, because the
encoder keys the field on its *length*. The mistake a mechanical translation
actually makes is to key it on the *pointer*, in all three places at once,
which writes an `ISFT` sub-chunk of length zero. Tested by hand before the
entry was rewritten: it grows the `emptySoftware` golden from 26,086 bytes to
26,094 — exactly the eight bytes of an empty sub-chunk — and the golden catches
it. The suite now lets one mutation name several sites, because some mistakes
are only mistakes when they are made consistently.

**"Each voice is rendered to the length of the utterance" is unobservable**, and
§18.5 has the measurement: all 744 voice sections end with their three
amplitudes at zero, so the extra samples are exact zeros and adding zero to a
float changes nothing. Marked `expect_caught=False` with the number beside it.

### Four expected misses, each proved rather than waived

| mutation | why it cannot be caught |
|---|---|
| JavaScript truthiness becomes C truthiness | the two differ only for a NaN `peakNormalize`, and every call passes either `KL_WAV_PEAK_NORMALIZE` or a literal `0.0` |
| each voice rendered to the utterance's length | all 744 sections end at zero amplitude, so the extra samples are exact zeros |
| an empty voice section is rendered anyway | no section compiles to zero events; every one ends with a trail-off |
| the default output path (POSIX entry) | that branch is inside `#else`, so on Windows it is not compiled. The expectation follows the platform rather than being waived on either |

All four are kept with `expect_caught=False`, so the claim is re-checked on
every run: if a NaN ever reaches the gain, or a section ever ends loud, or a
section ever compiles to nothing, the line turns into a failure instead of
staying quietly true.

### What it says about the attribution

Three separate mutations remove or alter the `ISFT` field — dropping it from
the corpus renders, dropping it from the CLI, and changing the string itself —
and all three are caught. The credit in the file is not protected by anybody
remembering it is there.

---

## 18.9 Four toolchains

`tools/build-matrix.ps1` gained stage 6, and stage 6 is the first stage where
the **program** is run on every toolchain rather than only its library: each
Windows leg passes its own `klattsch.exe` to the verifier with `--cli`, and the
WSL leg runs its `klattsch` on each end-to-end text inside a per-run directory
so that the line it prints is the same line the JavaScript prints on the
Windows side — the output path is part of that line.

The list of texts comes from `verify-stage6.mjs --list-cli-cases` rather than
being repeated in the shell script, so the two sides cannot disagree about
which texts they are comparing.

### Results

*Measured 2026-09-24 on the development machine, all four toolchains built and
their verifiers run in one command.*

| toolchain | libm | stage 6 | the CLI, end to end |
|---|---|---|---|
| MSVC | UCRT | all checks passed | 12/12 |
| clang-cl | UCRT | all checks passed | 12/12 |
| WinLibs gcc | UCRT | all checks passed | 12/12 |
| Debian gcc under WSL | **glibc 2.41** | all checks passed | 10/10 |

The WSL leg runs ten rather than twelve because the two extra invocations —
the default output path and the non-ASCII filename — need both programs in the
same place, and there is no Node in WSL.

And the dumps themselves, compared with `cmp`:

```
  wav 48000 Hz: identical on all four  (26,484,662 bytes)
  wav 22050 Hz: identical on all four  (12,220,822 bytes)
  wav 8000 Hz:  identical on all four  (4,497,542 bytes)
  round-sweep:  identical on all four
  tofixed-sweep: identical on all four
  wav-goldens:  identical on all four
```

**43.2 MB, byte for byte, across two C runtimes and three compilers.** The
glibc leg is the one that matters: it is the only independent libm here, and
it is the leg that disagrees with the other three at stage 1 (`glottalPulse`,
1.6% of its grid) and at stage 3 (three differing samples instead of four).
Neither disagreement survives into a file.

---

## 18.10 Step log

1. Read `src/engine/wav.js` and `bin/klattsch.mjs` and wrote down what the C
   would have to reproduce, including the two lines the CLI prints.
2. **Measured the normalization risk before writing any C** (§18.2), by
   putting stage 3's existing dumps through the CLI's pipeline on two libms.
   That is what turned the stage's exit test from a hope into a bar, and it
   cost ten minutes rather than the ten hours of discovering it afterwards.
3. Measured the reachable surface: 10 multi-voice cases, 5 that mix past full
   scale, 1 with a peak of exactly zero, 0 with an empty voice schedule, 0
   with a zero-length buffer, 3 that tie the byte count and 0 that tie the
   seconds figure.
4. Wrote `csrc/kl_wav.{h,c}`, `csrc/kl_render.{h,c}`, `csrc/kl_fmt.{h,c}`,
   `csrc/tools/kl_wav_dump.c` and `bin/klattsch_cli.c`.
5. Added the three encoder goldens step 3 showed were missing — `clipped`,
   `empty`, `emptySoftware` — and committed them on their own, before the C
   that would be compared against them.
6. Wrote `tools/verify-stage6.mjs`. It passed on the first run, at all three
   rates, which moved the suspicion to the verifier exactly as in stage 5.
7. Wrote `tools/stage6-mutations.py`, 44 mutations. 42 caught, 2 missed —
   and for the first time in this port neither miss was a gap in the corpus.
   Measured both, rewrote both, re-ran: 44 of 44. §18.8.
8. Extended `tools/build-matrix.ps1` to stage 6, including running the CLI
   itself on every toolchain, and added the suite to CI with `--rate 48000`
   so the job stays inside its timeout.
9. `ctest` 11/11, then the four toolchains. §18.9.

### What turned out to be wrong

- **REWRITE.md sized this stage “small, low risk”.** The size was right and
  the risk was not: peak normalization makes every byte depend on one sample.
  Corrected in place, with the measurement. §18.2.
- **“The CLI renders the whole corpus” was never possible.** `bin/klattsch.mjs`
  takes a phoneme string and nothing else, so it cannot reach the 262 cases
  that carry a bank, an engine or seeded extras. The exit test drives the
  pipeline and proves the CLI is that pipeline, separately. §18.7.
- **Stage 3's “zero differing samples after 16-bit quantization” did not
  transfer for free.** It was measured at gain 1. With a normalization gain
  the mapping changes, and it had to be measured again. §18.2.
- **Two of the mutations in the first suite were measuring nothing**, and
  both looked like corpus gaps until they were checked. One was a no-op —
  `strlen("")` is 0, so the two spellings of the empty-string guard are the
  same function — and one was genuinely unobservable. §18.8.
- **"Neither `toFixed` tie occurs in the corpus" was half wrong.** The seconds
  tie occurs in none of the 460 drivable cases; the byte-count tie occurs in
  three of them, at exactly 24.5 KB. The verifier used to *search* for them at
  9 s a run and then construct both anyway; it now names the corpus case and
  constructs only the one that really is absent. §18.3.
- **`execFileSync` returns stdout, and both programs write nothing there.**
  The end-to-end comparison silently compared `null` against `null` until it
  threw; it uses `spawnSync` and reads `stderr`.
- **A patch script's `\` was collapsed by the shell heredoc twice more**,
  the third and fourth times in this port. Once it turned a `
` inside a C
  string literal into a real newline, so the anchor matched nothing — caught
  immediately by the assertion on match counts. The second time it landed in
  a *PowerShell* file, where `"tools\verify-stage6.mjs"` became
  `"toolserify-stage6.mjs"` and then a **vertical tab**: the matrix ran for
  twelve minutes and only the WSL leg noticed, with
  `Cannot find module '…	oolserify-stage6.mjs'`. Raw strings are the fix;
  the file is now built with two `Join-Path` calls and no backslash at all.
- **An empty text list failed silently.** When that node call died, the
  generated shell script got a file containing one empty line, and the WSL CLI
  was run once with no argument — which prints usage and exits 1. The script
  now throws if `--list-cli-cases` produces nothing, because a comparison that
  runs zero comparisons should not look like a comparison that passed.
