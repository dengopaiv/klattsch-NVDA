# Making klattsch suitable for a screen reader

A synthesizer that renders a correct WAV file is not yet a screen-reader voice.
This document is the gap between those two things, and the proposals for
closing it.

The requirements below are not derived from klattsch. They are the hard-won
ones from building NVDA drivers for other engines in this tree, where each was
learned by hitting it: a say-all that stalls forever, a voice that goes
permanently silent while every call still returns success, an utterance that
bleeds into the one that cancelled it. Each is cheap to design in and expensive
to retrofit, which is why they are written down before the add-on is started
rather than after.

## 1. What klattsch already has that most engines do not

Worth stating first, because it changes what the add-on has to do.

**Exact index placement is free.** The compiler's `phrases` array already
carries `tStartMs` and `tEndMs` for every token
([ARCHITECTURE.md](ARCHITECTURE.md)). Engines that only emit a bookmark
callback force the driver to reconstruct index positions from byte offsets in
the audio stream and to reconcile them against playback position. klattsch
knows, before a sample is rendered, exactly when each token will be heard.
`IndexCommand` placement is a lookup. Do not throw this away by routing indices
through a generic callback — put the time directly in the schedule.

**The engine is in-process.** It is C, it is 64-bit, it links into NVDA's own
process. Everything a cross-process bridge needs — a wire protocol, a watchdog,
port allocation, a stdout drainer, restart-and-resume — does not exist here and
must not be invented. A whole category of failure is absent by construction.

**Cancellation can be immediate.** In-process and single-threaded, `cancel()`
can stop the render itself, not just stop consuming its output.

**Rendering is faster than real time by a wide margin**, and the engine has no
internal notion of time. It renders into a buffer at whatever rate the CPU
allows.

## 2. The requirements that are not optional

### 2.1 Every index fires exactly once, including the ones with no audio

NVDA's say-all emits chunks that are an `IndexCommand` and nothing else — no
speakable text. An engine given such a chunk produces no audio and therefore
no natural moment at which to report the index. Say-all then waits forever for
a `lineReached` that never arrives, and reading stops dead.

**Requirement.** Every index in an utterance fires exactly once: at its
playback position if it has one, and at utterance completion otherwise. An
utterance that is abandoned — cancelled, failed, empty — still fires its
pending indices and then `synthDoneSpeaking`. There is no path through the
driver on which NVDA is left waiting.

This is the single most common way an otherwise working synth driver is
unusable.

### 2.2 The audio thread must keep feeding even when idle

NVDA's `WavePlayer` checks its chunk-finished callbacks only when it is fed.
A driver that feeds audio and then goes quiet stops firing
`synthIndexReached` and `synthDoneSpeaking` altogether.

**Requirement.** The thread that owns the wave player flushes periodically
whether or not there is new audio. This is not a performance detail; without it
say-all stalls.

### 2.3 Cancellation is a generation counter, and control survives it

Audio for a cancelled utterance can still be in flight when the next one
starts. Dropping work by clearing a queue is not enough.

**Requirement.** Every utterance carries a generation id. `cancel()` bumps the
counter; any work item or audio chunk carrying an older generation is
discarded when it is reached. Control items — rate, pitch, voice, volume —
carry no generation, because a settings change must survive a cancel. This is
the design already in use in `votraxxion` and it should be copied rather than
re-derived.

### 2.4 Character mode and breaks need real implementations

`CharacterModeCommand` and `BreakCommand` are the two that engines commonly
lack, and the workarounds are known: spell mode speaks letter names rather
than passing text through the front end, and a break is synthesised as actual
silence of the requested duration.

klattsch is better placed than most here — `BreakCommand` is a `p<ms>`
directive the compiler already has, and silence is a real schedule event.
Neither needs a workaround; both need wiring.

### 2.5 Rate is constant-pitch

Two incompatible meanings of "faster" exist: shortening phoneme slots, and
running the engine faster. A screen-reader user turning up the speed does not
expect the voice to rise. klattsch's `rate` directive is the first, which is
the correct default and the only one worth offering.

Support `RateBoostSetting` as well — users who read at speed expect it, and it
is a second multiplier on the same `rate`, not a separate mechanism.

## 3. Latency, and the one design change it forces

This is the proposal with real consequences for the C engine, so it is the one
to decide early.

A screen reader is judged on **time to first audio**, not on throughput. The
current shape — `compileString`, then `renderToBuffer` for the whole utterance,
then play — makes that latency proportional to utterance length. Reading a
paragraph means rendering the whole paragraph before the first word is heard.

**Proposal: the C engine renders in chunks and the driver feeds as it goes.**

- `kl_synth` already renders into a caller-supplied buffer of any length and
  keeps all its state between calls. Chunked rendering needs no change to the
  DSP, only to the loop that drives it.
- Render one chunk (20–50 ms), feed it, render the next. Time to first audio
  becomes one chunk plus compile time, independent of utterance length.
- `cancel()` between chunks stops the render immediately rather than discarding
  work already done.

**Measure before assuming this is enough.** Two numbers decide it, and neither
is known yet:

1. **Compile time for a long utterance.** The compiler runs before any audio
   can be produced, so it is pure latency. It is a single pass over tokens and
   should be microseconds, but it has never been measured.
2. **Render speed as a multiple of real time.** klattsch recomputes three
   biquads' coefficients *every sample* during a transition — two `sin`, one
   `cos`, one divide per resonator per sample — and interpolates 19 parameters
   per sample. That is a lot of work per sample by the standards of this
   family. If it renders at only a few times real time, chunked rendering is
   mandatory rather than merely better.

Both go in the golden harness as timings, so a regression in either is caught
the same way a regression in the samples is. Neither is a reason to optimise
before measuring.

## 4. Failure modes to design against

From other drivers in this tree, in rough order of how badly each bites:

- **Silent success.** The engine accepts calls, returns success, produces
  nothing. Detect it: an utterance containing speakable text that yields zero
  samples is a fault, not a result. Distinguish that from a legitimate
  index-only chunk by checking whether speakable text remains after directives
  are stripped.
- **A worker thread that dies quietly.** Any thread that feeds a queue another
  thread waits on must enqueue a sentinel on its way out, or the waiter blocks
  forever with no error anywhere.
- **An unbounded front end.** Text arrives from a screen reader in unpredictable
  sizes. Fixed-capacity buffers with explicit bounds, and a documented
  truncation behaviour, rather than growth on the speech path.

## 5. What to expose, and the order to decide it

The generator exposes everything ([GENERATOR.md](GENERATOR.md)). The add-on
must not: a settings panel with thirty sliders is worse than one with eight.

**Proposal: measure each parameter's audible effect before deciding it deserves
a control.** This is the method used on another engine in this tree, where
measuring the full parameter row one at a time found that several parameters
did nothing at all, and several others worked in one direction only. Shipping a
slider for a parameter nobody has listened to is how a settings panel fills
with controls that do not work.

For klattsch that means the roughly thirty frozen constants of
[ARCHITECTURE.md](ARCHITECTURE.md) get measured — one at a time, rendered
across their range, before any of them reach a user-facing control. The
measurement is a document, and it decides three lists:

| List | Goes where |
|---|---|
| Audible and useful in ordinary speech | NVDA settings panel, and in the settings ring |
| Audible but for voice design only | The generator |
| Inaudible, or broken in one direction | Documented as such; no control anywhere |

Everything in the first list is marked `availableInSettingsRing=True`. A
parameter reachable only through a dialog is, in practice, a parameter most
users never touch.

The starting proposal for the first list, to be confirmed by measurement:
rate, pitch, volume, inflection, breathiness (`aspiration`), effort, head size
(`scale`), and the phoneme bank.

## 6. Voices are presets, and the two programs share the format

A "voice" in NVDA's list is a named bundle of engine parameters. The generator
writes exactly that format ([GENERATOR.md](GENERATOR.md)) — a base to start
from plus the values that differ from it — and the add-on reads it. A voice
designed in the generator appears in NVDA's voice list with no conversion step.

This is the reason to build both programs against one engine, and it is worth
protecting: if the two formats ever drift, the generator stops being a tool for
making voices and becomes a toy.

## 7. Build and verification

- **CMake**, one build for the library, the CLI and the tests, per house rule
  §4. The stage harnesses of [REWRITE.md](REWRITE.md) keep their own scripts;
  the product is assembled once, from the same sources.
- **One version constant**, in the public header, parsed by everything else
  that needs it — CMake, the add-on manifest, the release script.
- **Built by more than one compiler, on more than one OS.** MSVC and clang-cl
  on Windows, gcc on Linux, all producing the same samples. A port verified on
  one compiler is verified against that compiler's arithmetic, not against the
  reference.
- **The goldens run in `ctest`**, labelled so the fast subset can run on every
  build and the full corpus on demand.

## 8. Order

This slots into [ROADMAP.md](ROADMAP.md) without changing its phases.

- [ ] **A.** Measure compile time and render speed (§3). Two numbers, into the
      golden harness. Do this as soon as the C engine renders — it decides
      whether chunked rendering is mandatory.
- [ ] **B.** Chunked rendering in the C API (§3), with cancel between chunks.
- [ ] **C.** The parameter audibility measurement (§5), producing the three
      lists and a document.
- [ ] **D.** The driver, against §2 as a checklist — every index fires once,
      idle flush, generation counter, character mode, breaks, constant-pitch
      rate.
- [ ] **E.** Test with NVDA actually running, including say-all over a long
      document, cancel under load, and the settings ring.

§2 is a checklist rather than prose on purpose. It is the list to re-read when
the driver is written, and again when it misbehaves.
