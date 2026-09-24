# Stage 5 — `kl_compile.c`, the schedule compiler

Translated from `compileSection()` and `compile()` in `src/engine/sequencer.js`.
klattsch is Tony Gies's work; this is a translation of part of it and carries
the same MIT notice.

[REWRITE.md](REWRITE.md) calls this the only high-risk stage in the plan, for
three reasons: it is the largest translation, it is the only stage where a
difference is a *logic* difference rather than a numeric one, and it is where
most of the hazards written down before the port began actually live. It sits
after stage 3 so that the sample loop — the part that is hard to debug by
reading — was already known good before the compiler went under test.

**Exit test:** Tier 1 on the whole corpus. Event count, `atMs`, `transitionMs`
and every target field compared as exact IEEE-754 doubles; warning strings
identical; phrase spans identical; and the schedule and phrase digests equal to
the frozen goldens, per voice. ✅ Passes.

> **Measured on four toolchains.** MSVC, clang-cl, WinLibs gcc and WSL Debian
> gcc, with `tools/build-matrix.ps1` — which this stage extended to cover
> stages 3, 4 and 5, because until now it ran the stage 1 and 2 verifiers only
> and the later stages were checked per toolchain by hand. See §17.9.

---

## 17.1 What the compiler actually is

A cursor over the token list, and nine running scalars.

The tokens arrive already classified by stage 4. What this stage adds is
*state*: a running fundamental, rate, formant scale and six voice-quality
values, each of which a directive can set absolutely, nudge relatively, or
reset to the value the caller supplied. Phonemes are looked up in the bank
that is current at that moment, turned into one or two schedule events, and
time advances.

Four things come out:

| | |
|---|---|
| `schedule` | the events a `FormantSynth` consumes — `atMs`, `transitionMs`, and a target |
| `phrases` | source spans with the time each sounds for, which is what highlights text |
| `warnings` | strings, and they are as much the contract as the numbers are |
| `totalMs` | the maximum across voice sections, not voice 0's |

The C produces `kl_event` directly — the same struct `kl_synth_queue()` already
takes — so stage 6 can hand one to the other without a conversion step. That
was the point of doing stage 3 first.

### Storage

No `malloc`, like the rest of the port, so the NVDA driver can compile an
utterance on its synth thread without an allocator in the path.
`kl_compile_need()` sizes every buffer from the token list.

It takes the token list rather than a length, and that is deliberate. Every
other bound in this port can be derived from the input length, but the extras
pool cannot: an upper bound of "one snapshot per event × one key per token" is
quadratic in the input, which for a thousand-token utterance is sixteen
megabytes of arena for a feature almost nobody uses. Counting the actual
uppercase directives instead gives an exact bound, which for real input is a
few dozen bytes.

Two of the buffers are scratch rather than result — the live extras set, and
the phonemes buffered inside `( … )` until the group closes. C17 has no
variable-length arrays to fall back on and this port has no allocator, so the
caller owns even the temporaries. Saying so in the header is better than
hiding a fixed cap inside the implementation.

---

## 17.2 The three layers of `emit()`, and the one that is observable

Every target is built by the JavaScript as

```js
target: { ...target, ...extras, ...stateExtras() }
```

Three layers, each overwriting the one before it. The C reimposes that as an
assignment order, because a struct has no spread. [REWRITE.md](REWRITE.md)
listed this as a hazard before the port began — "property order in the emitted
target … has to be reimposed deliberately" — and it was right to.

**Layer 2 over layer 1 is real and reachable.** An extras key is anything the
directive switch does not recognise and whose name begins with an uppercase
ASCII letter. Ten of the nineteen synthesis parameters have such names:

```
F0  F1  F2  F3  BW1  BW2  BW3  A1  A2  A3
```

So `[F1=900] AA` does not add an extra field called `F1` — it *overwrites the
phoneme's first formant*, and the running formant scale never touches it:

```
[F1=900] AA
  event 0   F1=900   F2=1220  F3=2600   (the phoneme's own F1 of 700 is gone)
  event 1   A1=0 A2=0 A3=0  F1=900      (and it rides into the silence too)
```

That second line is the part a translation gets wrong. A silence event carries
`{A1:0, A2:0, A3:0}` and nothing else — until an extras key names a parameter,
at which point that parameter becomes *present* on a silence event that would
otherwise never mention it.

**Layer 3 over layers 1 and 2 is not observable, and that was measured rather
than assumed.** The seven voice-state parameters are `vibratoDepth`,
`vibratoRate`, `tremoloDepth`, `tremoloRate`, `aspiration`, `tilt` and
`effort`. Every one of them begins with a lowercase letter, and an extras key
can only begin with an uppercase one, so the two key spaces cannot intersect.
No shipped bank defines a phoneme field with one of those names either. The
ordering between layer 3 and the rest is therefore correct by construction and
untestable — which the mutation suite records as an expected miss rather than
quietly not testing (§17.7).

### `[F4=3300] s1.2` does not scale F4

`scaled()` has a loop that multiplies any `F4`…`F9` or `BW4`…`BW9` the *phoneme*
defines by the running formant scale. No shipped bank defines one, so the loop
is dead for the three banks that exist. And the corpus case named
`extras/scaled-f4` does not reach it either, because `[F4=3300]` puts F4 in
`extras`, not in the phoneme — so it rides through unscaled:

```
[F4=3300] [BW4=250] s1.2 AA
  F1=840 (700 × 1.2)   F4=3300 (unscaled)   BW4=250 (unscaled)
```

The case name says the opposite of what the case proves. Left as it is, because
the behaviour it pins is real and worth pinning; recorded here so the name does
not mislead the next reader.

---

## 17.3 The four shapes of `renderPhoneme`

In order, and the order is the contract: stop, then glide, then pitch move,
then steady. A phoneme that was both a stop and a diphthong would take the stop
shape. None is, but a translation that reorders them is wrong regardless.

| shape | events | time |
|---|---|---|
| **stop** | silence, then a burst | `silenceMs` then `burstMs`, where `burstMs = min(25, slot×0.3)` |
| **glide** | onset, then the endpoint | `slot×0.25` then `slot×0.75` |
| **pitch move** | start pitch, then end pitch | `slot×0.25` then `slot×0.75` |
| **steady** | one event | `slot` |

### One of the caps is dead code in the reference

The stop's burst event is emitted with `Math.min(5, burstMs * 0.2)`. That `5`
can never bind:

```
burstMs = Math.min(25, slotMs × 0.3)   ⇒   burstMs ≤ 25
                                       ⇒   burstMs × 0.2 ≤ 5
```

Checked over 402,001 slot widths from −500 ms to 100,000 ms — far outside
anything the grammar can produce — and the cap changed the answer **zero**
times. Raising it to 6 therefore cannot move a schedule, whatever the corpus
contains. The mutation suite carries that mutation with `expect_caught=False`
and the arithmetic above, so if the surrounding code ever changes such that the
cap *can* bind, the suite says so instead of staying quietly true.

This is the second dead constant the method has turned up; stage 3 found
`atSample` truncating rather than flooring on a value that is never negative.
Neither is a bug. Both are places where a test would otherwise have been
proving nothing while looking like it proved something.

### An unknown phoneme does not consume time

`renderPhoneme` warns and returns without emitting, and without advancing the
clock — but the caller still records a phrase and still applies the pitch
delta. So `AA ZZZ AA` produces a phrase of zero duration in the middle, and
`ZZZ+15` still moves the running pitch for everything after it. Reproduced
literally.

---

## 17.4 Sections, and the state that must not carry

`compile()` splits the token list at each `[voice=N]` marker. Sections are
positional — text before the first marker is voice 0 — and each compiles from a
**fresh initial state**. That is the easiest thing in the whole compiler to get
wrong: a running `f0` that leaks across a marker is invisible in any single
utterance and wrong in every duet.

The markers are removed by `compile()` before `compileSection()` sees them,
which is why the compiler never warns `unknown directive: voice` even though
`voice` is not in its switch. A C port that compiled the whole token list in
one pass would emit that warning and be wrong in a way the warning strings
catch.

Three top-level fields are not voice 0's:

- `totalMs` is the **maximum** across sections,
- `warnings` are every section's, concatenated in section order,
- `engine` and `phrases` *are* voice 0's.

---

## 17.5 What the golden harness was digesting as NaN

Found while reading the digest to work out what the C had to reproduce, before
any C was written.

`scaled()` spreads the whole phoneme object into the target, so every field a
bank defines rides along. Both Japanese banks give their phonemes `ipa` and
`example`, and `ja-mokhtari-2000` gives some a `source` — **strings**. The
schedule digest sorted them into its "engine-specific extras" list and called:

```js
d.f64(evt.target[k])      // → this.b.writeDoubleLE("i", 0)
```

`Buffer.writeDoubleLE` of a string does not throw. It writes a quiet NaN,
`00 00 00 00 00 00 f8 7f`. So 28 corpus cases — every case touching a Japanese
bank — were pinning the sorted positions of two documentation strings and a
NaN, and nothing about synthesis.

Confirmed before changing anything: the checked-in digest for `bank/switch`
reproduces exactly under the old rule and not under the new one. That is what
identified it, rather than a guess about what the code looked like it did.

The fix is one line in the set `goldens.mjs` already keeps for exactly this —
the "one documented difference between the two schedules … in one place, rather
than as a tolerance scattered through the comparison". It knew about `isStop`
and `glideTo`, because whoever wrote it was looking at `klatt1980-en`, which has
no per-phoneme documentation. The Japanese banks do.

Excluding the three names cannot mask a real directive, and that is provable
rather than likely: an extras key is only ever created by the `/^[A-Z]/` branch
of the directive switch, so every extras key begins with an uppercase ASCII
letter, and `ipa`, `example` and `source` do not.

Re-captured. **686 of 714 cases byte-identical.** The 28 that moved differ in
`compile.scheduleDigest` and the per-voice copy of it, and in no other field —
not `totalMs`, not `warnings`, not `phrasesDigest`, and not either audio
digest, which is independent confirmation that the strings never reached the
sample loop.

---

## 17.6 The exit test

`tools/verify-stage5.mjs`, registered as the `stage5-compile` ctest entry.

The C dump emits raw fields rather than digests, so a mismatch reports as
*case, voice, event, parameter* instead of as two hex strings that differ. Four
sections, in the order a failure is easiest to read in:

| section | what it compares |
|---|---|
| the top level | `totalMs`, voice count, merged warning strings, engine marker |
| the schedule | every event's `atMs`, `transitionMs`, each of the 19 parameters' presence *and* value, and the sorted extras |
| the phrases | source offsets, start and end times, kind, phoneme |
| the goldens | schedule and phrase digests, per voice, against `goldens/cases.json` |

The first three compare against a **live** `compile()`, which says what differs.
The fourth compares against the **frozen** goldens, which ties the C to the
reference as captured rather than to whatever the JavaScript does today. Stage 1
learned that the first without the second is a comparison with no reference in
it.

Everything is `Object.is` or `===`. No tolerance anywhere. REWRITE.md's Tier 1
allowed `noteToHz` to be compared to within 1e-9 Hz as "the one declared
exception"; that turns out not to be needed here, because `noteToHz` is spent
in the *tokenizer* and by the time a value reaches this stage it is a number the
token already carried. Stage 5 is exact on every field.

### The blob cannot go stale unnoticed

`goldens/cases-compile.bin` carries each case's text *and its opts*, because 262
of the corpus cases set a bank, an engine or seeded extras, and a compiler
driven by text alone would never reach those paths.

It is a checked-in derived file, so the obvious worry is that it drifts from
`cases.json`. It cannot drift silently: the verifier walks both by index and
compares the C's answer for the blob's case *i* against the JavaScript's answer
for `cases.json`'s case *i*. A stale text, a stale opt or a missing case all
show up as a mismatch or as an explicit case-count error.

### Numbers

```
729 cases over 20 groups
  totalMs exact                     729/729
  voice count                       729/729
  warning strings identical         729/729
  engine marker                     729/729
  event counts                      744/744 voices
  every target field exact          53460 fields over 2430 events
  phrase spans identical            1445 phrases
  schedule digest (voice 0)         729/729
  phrases digest (voice 0)          729/729
  schedule digest, every voice      744/744
```

---

## 17.7 The mutation suite, and the five gaps it found

**The stage 5 verifier passed on its first run.** For the highest-risk stage in
the plan that is the least reassuring way to pass, and it is exactly the
situation the meta-test exists for: a test that cannot fail is worse than none.

`tools/stage5-mutations.py`, 51 mutations in seven groups, each one a single
literal replacement in `csrc/kl_compile.c` that must make the verifier fail.

One improvement on the stage 3 and 4 suites: **each pattern must occur exactly
once.** Those suites replaced the first match, which is silently the wrong edit
the moment a later change makes a pattern ambiguous — and reports whatever that
edit does as the result for a line it did not name.

### First run: 43 caught, 8 missed

Three of the eight are unreachable, and each was proved rather than argued:

| mutation | why it cannot be caught |
|---|---|
| voice state applied before the extras | extras keys start uppercase; all seven voice-state names start lowercase. The key spaces are disjoint. |
| stop burst transition cap 5 → 6 | `burstMs ≤ 25 ⇒ burstMs × 0.2 ≤ 5`. Scanned 402,001 slot widths; the cap bound zero times. |
| reset and relative swap precedence | no token is ever both. `classifyPart` sets `reset` only on the bare forms and `relative` only on the signed compact form. Measured: 455 directive tokens in the corpus, 0 with both. |

Each is kept with `expect_caught=False`, so the claim is re-checked on every
run and turns into a failure if it ever stops being true.

**The other five were real holes in the corpus**, and every one of them is a
path a shipped engine would use:

1. **An extras key naming a parameter.** Nothing in 714 cases wrote `[F1=900]`,
   so the compiler could stop honouring the collision entirely and no golden
   moved. → `extras/names-a-parameter`, `extras/names-f0`,
   `extras/names-a-parameter-on-silence`.

2. **`[pitch=N]`.** The only way to reach the `pitch` key — no compact letter
   maps to it — so `base` and `pitch` could be separated and nothing noticed.
   → `directive/pitch-bracket`, `directive/pitch-then-base`.

3. **Bare-letter reset returning to the caller's value.** This is the big one.
   Of the 262 cases carrying opts, every single one carried `bank`, `extras`,
   `engine` or `gain` — **not one set a scalar**. So all ten of
   `opts.baseF0 ?? 120`, `opts.rate ?? 110` and the rest could take the default
   instead of the caller's value and the whole corpus stayed green. A screen
   reader sets rate and pitch on every utterance; this is the path the shipped
   engine will spend its life in. → a new `opts` group of seven cases,
   including one that resets all ten in a single line so no single wrong
   initial value can hide behind the other nine.

4. **`[bank]` resetting to the caller's bank.** `bank/reset-to-opts` existed
   and could not tell the three answers apart: it reads `AA` after the reset,
   and `AA` is inherited unchanged from `klatt1980-en` by both Japanese banks,
   so the opts bank, the default bank and the previous bank all agree. `A`
   exists only in the Japanese banks and differs between them. →
   `bank/reset-to-opts-distinct`.

5. **Warnings from a later voice section.** Every other voice case compiles
   cleanly, so voice 0's warnings could be reported as the whole merged list.
   → `voice/warning-in-second`, `voice/warnings-in-both`.

Corpus: **714 → 729 cases, 19 → 20 groups.** Every addition was driven by a
surviving mutation, never by a stage needing something to pass.

### Second run

```
caught 51, missed 0
All mutations caught.
```

The suite counts an expected miss as a pass, so that 51 is 48 mutations caught
outright plus the three above, each of which printed `not caught, as expected`
on its own line. Nothing slipped through unaccounted for.

---

## 17.8 A generator gap, closed

`tools/build-banks-c.mjs` reads a fixed set of phoneme fields, and read
`glideTo.F1`, `glideTo.F2` and `glideTo.F3` without checking that those were
all a `glideTo` contained. They are, across all 13 glide entries in all three
banks — but nothing was checking, and the C compiler was written *relying* on
it: `scaled()` takes the bandwidths and amplitudes from the phoneme even on the
glide event, which is only correct while `glideTo` cannot carry them.

A bank adding `glideTo.BW1`, or any phoneme field outside the known set, would
have been silently dropped from the C tables while the JavaScript kept
spreading it into every schedule target. That is a real engine difference, not
a cosmetic one.

The generator now refuses, and the refusal names the file to change:

```
ja-hecko-2026.IY.glideTo: unknown field "BW1". kl_glide holds F1, F2 and F3
only, and csrc/kl_compile.c relies on that: it takes the bandwidths and
amplitudes from the phoneme even on the glide event, which is only correct
while glideTo cannot carry them.
```

Both guards were **tested by injecting a field and watching them fire**, not
just by observing that the current banks pass. The first attempt at that test
did not fire at all — because the generator reads `bundled.js`, not the JSON,
so editing the bank source proved nothing until `tools/build-banks.js` had
regenerated the bundle. A guard verified the wrong way is a guard nobody should
trust.

---

## 17.9 Four toolchains, and the script that had fallen behind

`tools/build-matrix.ps1` covered **stages 1 and 2 only**. Stages 3 and 4 were
built and verified per toolchain by hand, which is worse than a script and was
recorded as a weakness in [ROADMAP.md](ROADMAP.md) rather than left implied.
Doing stage 5 by hand again would have repeated it.

The script now covers stages 1 to 5:

- stage 4 needs two dump tools, so the per-stage table gained an optional
  second tool rather than a special case in the loop;
- the WSL leg — the only glibc build on this machine, and the only one whose
  libm can legitimately disagree — now dumps every stage's binaries into one
  directory and runs each verifier against it in directory mode;
- the sample rates it dumps come from `goldens/manifest.json` rather than a
  list repeated in the script, so a rate added to the corpus cannot silently
  stop being covered on the leg that matters most.

Stage 2 is skipped on the WSL leg, and needs no apology for it: it is pure
table data with no arithmetic a second libm could answer differently.

### Results

| toolchain | runtime | stage 5 |
|---|---|---|
| MSVC 19.51 | UCRT | ✅ |
| clang-cl | UCRT | ✅ |
| WinLibs gcc | UCRT | ✅ |
| WSL Debian gcc | glibc 2.41 | ✅ |

Not just "passes on all four" — the dumps are **byte-identical**. All four
builds produce the same 483,678 bytes, `cmp` clean against the glibc leg:

```
build-msvc  vs wsl: byte-identical
build-clang vs wsl: byte-identical
build-gcc   vs wsl: byte-identical
```

Stage 5 is Tier 1 in fact as well as in intent. Unlike stage 1 — where
`glottalPulse` disagrees between UCRT and glibc in the last bit and needs the
Tier 2 tolerance — there is no arithmetic here for a libm to answer
differently.

Fixing the script turned up one thing worth recording, because the error
message named nothing useful. Written inline, PowerShell binds the operands of
`-replace` as further arguments to the method call:

```powershell
[IO.File]::WriteAllText($shFile, (($lines -join "`n") + "`n") -replace "`r`n", "`n")
# Cannot find an overload for "WriteAllText" and the argument count: "3".
```

Split into two statements. The WSL leg had silently produced no dump at all
until then, and the three Windows toolchains had still reported five green
stages each — which is exactly the shape of a matrix that looks kept and is
not.

---

## 17.10 Step log

1. Read `compileSection()` and `compile()` against the golden digest to work
   out exactly what the C had to reproduce — presence as well as value, and the
   sorted extras list.
2. Found the NaN digest (§17.5) before writing any C. Confirmed it against the
   checked-in golden, fixed it in the one place the harness keeps for it,
   re-captured, and verified that only the 28 Japanese-bank cases moved and
   only in their schedule digests. Committed on its own.
3. Probed the reference for the four behaviours a struct-based port could get
   wrong: extras over a parameter, extras on a silence event, `F0` overridden,
   and `F4` left unscaled. Three of the four were uncovered by the corpus.
4. Wrote `kl_compile.h` / `kl_compile.c`, `csrc/tools/kl_compile_dump.c`, and
   extended `tools/cases-to-bin.mjs` to carry each case's opts.
5. Wrote `tools/verify-stage5.mjs`. It passed on the first run, which moved the
   suspicion to the verifier.
6. Wrote `tools/stage5-mutations.py`, 51 mutations. 43 caught, 8 missed.
7. Proved three of the misses unreachable and marked them; closed the other
   five with 15 new corpus cases and a new `opts` group.
8. Re-ran: 48 caught, 0 missed. `ctest` 10/10.
9. Closed the `build-banks-c.mjs` glideTo gap (§17.8) and tested both guards by
   making them fire.
10. Extended `tools/build-matrix.ps1` to stages 3, 4 and 5, fixed the WSL leg
    it broke, and ran the four toolchains (§17.9). All four dumps
    byte-identical.

### What turned out to be wrong

- **The golden harness was digesting two strings as NaN** on 28 cases. §17.5.
- **`extras/scaled-f4` does not test what its name says.** §17.2.
- **The corpus set no scalar opt at all**, leaving all ten `opts.x ?? default`
  paths unverified. §17.7.
- **`bank/reset-to-opts` could not distinguish the three banks** it was written
  to distinguish. §17.7.
- **The first test of the new generator guard proved nothing**, because it
  edited the bank JSON while the generator reads `bundled.js`. §17.8.
- **A patch script's `\\` was collapsed by the shell heredoc**, turning
  `tools\verify-stage1.mjs` into a string containing a vertical tab and making
  an anchor silently fail to match. The same trap as stage 4, and the assertion
  on match counts is what caught it again.
