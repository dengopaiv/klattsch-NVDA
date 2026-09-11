# The C rewrite

## Why C and not C++

The survey in `..\speech synths overview.md` recommends modern C++ for a new
synthesis project. This is not a new project, and the argument that decided
`votraxxion` applies here unchanged:

1. **One toolchain.** Plain C11 builds under MSVC, MinGW, clang and gcc with no
   C++ ABI questions. The NVDA add-on needs x86 and x64 Windows libraries today
   and a Linux build the day a Linux screen reader wants it.
2. **No allocation on the speech path.** A synthesizer feeding an audio callback
   should never take the allocator lock. Fixed-capacity buffers with explicit
   bounds, sized above anything a screen reader hands over in one call.
3. **Size.** Template instantiation and exception tables for a program that
   never throws. The `votraxxion` C library is 153 KB per architecture; that is
   the target shape.

And the shape of the thing agrees. `FormantSynth` is nine scalars, three biquad
structs and a loop. `compileSection` is a cursor over a token array. Neither
wanted a class in JavaScript and neither will want one in C.

The one place C++ would genuinely help is the GUI, and the GUI is a separate
binary that can be whatever it likes. See [GENERATOR.md](GENERATOR.md).

## The acceptance criterion

`votraxxion`'s rule was **byte-identical output or it is a bug**, and it could
hold that rule because the C++ it replaced and the C that replaced it both ran
on the same machine with the same libm.

**That rule cannot honestly be claimed here, and pretending otherwise would
hide real bugs behind a tolerance.** klattsch's DSP calls `Math.sin`, `Math.cos`
and `Math.pow` every sample, and V8 implements those with its own
fdlibm-derived code rather than the platform libm. Two correct implementations
will differ in the last bits. So the criterion is split:

**Tier 1 — exact, no tolerance.** Everything that is integer or rational
arithmetic must match bit for bit:

- the tokenizer's classification of every token in the corpus,
- the compiler's schedule: event count, `atMs`, `transitionMs`, and every
  target field, compared as exact IEEE-754 doubles,
- the xorshift LFSR state sequence, all 2^32 - 1 states if you like, but at
  minimum the first million from the seed `0xACE1ACE1`,
- sample counts, WAV header bytes, and the `ICMT` round-trip.

A single differing schedule field is a bug, not a rounding difference. The
compiler does no transcendental arithmetic except `noteToHz`, which is the one
declared exception and is compared to within 1e-9 Hz.

**Tier 2 — bounded, and the bound is tight.** Rendered audio, compared against
the JS reference:

- peak absolute sample difference `<= 1e-9` on a 30-second corpus, in float64
  before quantization,
- after 16-bit quantization, **zero differing samples**. A 1e-9 float
  difference cannot move a 16-bit sample unless it sits exactly on a rounding
  boundary; if any sample differs, that is investigated, not waived.

If Tier 2 cannot be met, the cause is a logic difference, not libm. Every time
this has been tested on a formant synth the transcendental error has stayed
five orders of magnitude below the 16-bit LSB.

**Goldens are captured from the JS before the first line of C is written**, by
`tools/goldens.mjs`, and checked in. The corpus has to be adversarial about the
things a rewrite actually breaks:

- every phoneme in all three banks, in isolation, and in a vowel frame,
- every directive, in absolute, relative, and bare-reset form,
- note-name pitch across the full `A-1` to `G#9` range,
- both pitch-delta forms, sticky and transient, at boundaries,
- syllable groups including the malformed ones: nested `(`, unmatched `)`,
  unclosed `(`, each of which has a defined behaviour in the JS that the C must
  reproduce,
- bank switching mid-utterance and `[bank]` reset,
- `[voice=N]` with 1, 2 and 5 sections, including an empty section,
- uppercase extras set, overridden and cleared,
- unknown phonemes and unknown tokens — the warning strings are part of the
  contract,
- comments in every position, including a block comment splitting a token,
- homoglyph and zero-width normalization,
- `rate` low enough that the `min()` caps in `renderPhoneme` bind, and high
  enough that they do not,
- the empty string.

## The shape

`csrc/`, C11, one translation unit per concern, each with a header:

| File | From | Notes |
|---|---|---|
| `kl_dsp.{h,c}` | `src/engine/dsp.js` | Biquad, glottal pulse, xorshift, soft clip. Mechanical, but the LFSR needs care — see below. |
| `kl_synth.{h,c}` | `src/engine/synth-core.js` | The sample loop. `PARAMS` becomes a fixed struct, not a map. |
| `kl_banks.{h,c}` | `src/engine/banks/*` | Banks compiled in as static tables, generated from the JSON by `tools/build-banks-c.mjs` — the same JSON stays the source of truth, and the JS and C banks can never drift. |
| `kl_token.{h,c}` | `tokenize()` | Normalization tables, comment stripping, classification. |
| `kl_compile.{h,c}` | `compileSection()`, `compile()` | The cursor and the four phoneme shapes. Schedule into a caller-provided arena. |
| `kl_wav.{h,c}` | `src/engine/wav.js` | RIFF/WAVE with the LIST INFO chunk. |
| `klattsch.{h,c}` | `src/engine/index.js` | The public C API and the one-call convenience path. |

`bin/` gains `klattsch_cli.c`, the C equivalent of `bin/klattsch.mjs`, which is
what proves the engine before any add-on or GUI exists.

The JavaScript stays. This fork does not delete upstream's engine — it is the
reference implementation that the goldens come from and that regressions are
measured against, permanently.

## The numeric hazards, written down before they bite

Each of these is a place where a mechanical translation is wrong.

**The LFSR is signed.** `xorshift` uses `x >>> 17`, a *logical* shift on the
unsigned reinterpretation, while `x << 13` and `x << 5` are wrapping signed
shifts, and `noiseSample = lfsr / 2147483648` divides the **signed** int32. In
C: do the shifts in `uint32_t`, then reinterpret to `int32_t` for the divide.
Getting this wrong gives noise in [0, 2) instead of [-1, 1) and a DC offset
through every fricative.

**Everything is double, output is float.** JavaScript arithmetic is float64
throughout; only the store into `Float32Array` rounds. The C loop must compute
in `double` and round once, on the store to `float`. A `float` accumulator in
the biquads will drift audibly over a long utterance.

**`Math.floor` on negatives.** Both phase wraps use `x -= floor(x)`. C's
`floor` agrees, but an `(int)` cast does not — it truncates toward zero. Use
`floor`.

**Coefficient caching is observable.** `BandpassBiquad.setFreq` returns early
when `(f, bw)` are unchanged *before* clamping, so the cache key is the raw
values and the clamp is applied after. During a transition the values change
every sample and the cache never hits; when a parameter is held, it always
hits. Reproduce the ordering exactly or the clamped-range behaviour differs.

**Property order in the emitted target.** The JS builds each target as the
phoneme fields, then `extras`, then voice state, so a bank that defined
`aspiration` would be overwritten by the running `aspiration`, and an
uppercase extra is overwritten by nothing. With a fixed struct this ordering
has to be reimposed deliberately as an assignment order.

**Stray fields ride along.** `scaled()` spreads the whole phoneme object, so
`isStop` and `glideTo` end up inside schedule targets. `FormantSynth` ignores
them because it iterates `PARAMS`. The C struct simply will not have them —
which is correct, and is a difference the golden comparison must be told to
expect, in exactly one place, rather than silently tolerating extra keys.

**`0.1 + 0.2`.** Time accumulates as repeated `timeMs += slotMs` in float64.
Reproduce the accumulation order exactly; do not "improve" it by multiplying an
index by a slot width.

## The order

Bottom-up, so each step is verifiable before the next depends on it.

- [ ] **0. Baseline.** `tools/goldens.mjs` against the JS engine; corpus above;
      goldens checked in with their generator.
- [ ] **1. `kl_dsp.c`.** Biquad, pulse, LFSR, soft clip. Checked against the JS
      per function: a million LFSR states exactly, biquad coefficients across
      the frequency/bandwidth grid, the pulse across phase × effort.
- [ ] **2. `kl_banks.c` + `tools/build-banks-c.mjs`.** Generated from the same
      JSON as `bundled.js`. Checked field by field across all three banks, with
      `extends` resolution and `null` deletion exercised.
- [ ] **3. `kl_synth.c`.** The sample loop, driven by a golden schedule read
      from JSON so the compiler is not yet in the picture. Tier 2 comparison.
- [ ] **4. `kl_token.c`.** Classification of every corpus token, exact.
- [ ] **5. `kl_compile.c`.** The four shapes, directives, syllables, voices,
      banks, extras, warnings. Tier 1 comparison, exact.
- [ ] **6. `kl_wav.c` + `bin/klattsch_cli.c`.** End to end: the CLI renders the
      whole corpus and every WAV is byte-identical to the JS CLI's.
- [ ] **7. Extensions, each off by default.** Formant count as a build
      dimension, `FNZ`/`FNP`, jitter/shimmer/flutter, per-phoneme duration,
      cascade path. Each lands with the goldens re-run to prove the default
      path is unchanged — extensions that alter the baseline are bugs.
- [ ] **8. Regression.** The goldens run in CI, on every commit, forever.

Steps 0–6 are the rewrite. Step 7 is the part that makes it worth having done,
and nothing in step 7 begins until step 6 is green.

## Step log

Kept short on purpose: what changed, and what proved it.

*(empty — step 0 has not started)*
