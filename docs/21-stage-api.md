# The streaming API — what the NVDA add-on calls

Step 5 of [NVDA-ADDON.md](NVDA-ADDON.md). The engine, the compiler and the
text front end were finished and verified; the generator strung them
together for a whole utterance. A screen reader needs two things that path
does not have: the first sample before the last one is rendered, and NVDA's
index markers placed on the audio. This stage adds those two things, as a C
API and a shared library, and changes no sample the goldens pin.

**Exit test**, the five parts of NVDA-ADDON.md step 5:

1. the chunked stream equals `kl_render_mix` bit for bit — ✅ 729 corpus
   cases × 5 rate/chunk pairs, 33,572,040 samples, 0 differing;
2. every index comes out exactly once, in order, including index-only and
   empty utterances and a cancel at every chunk boundary — ✅;
3. speakable corpus text never compiles to zero samples — ✅ 32 of 32 lines
   with letters or digits;
4. `text-frontend` unchanged with inflection at 1 — ✅;
5. the C engine's time to first chunk and real-time ratio, measured — ✅
   §21.7.

Parts 1–4 pass on MSVC, clang-cl, WinLibs gcc and WSL Debian gcc (glibc)
through `tools/build-matrix.ps1`, measured 2026-10-28, and on Ubuntu gcc in
CI from this stage on. `tools/api-mutations.py` breaks the API 22 ways: 18
are caught, and the 4 that are expected to survive do, each for a stated
reason (§21.5).

---

## 21.1 What was built

| File | What |
|---|---|
| `csrc/klattsch_api.h`, `csrc/kl_api.c` | The API: settings; text, spelling, raw source and breaks in, with indexes between; 16-bit chunks out; cancel |
| `csrc/kl_stream.h`, `csrc/kl_stream.c` | `kl_render_mix`, one chunk at a time. Internal; the API's only rendering path |
| `csrc/kl_text.h`, `csrc/kl_text.c` | One new option, `inflection`: a scale on the sentence contour |
| `csrc/tools/kl_api_test.c` | The harness: `stream`, `api`, `level`, `bench` |
| `tools/api-mutations.py` | 22 mutations of the above |
| `CMakeLists.txt` | `klattschNative`, the shared library; ctest `api-stream`, `api-behaviour` |

The call sequence is the one at the top of `klattsch_api.h`: create a handle
at a sample rate, set what differs from the defaults, `begin`, add pieces and
indexes in NVDA's order, `end`, then call `render` until it returns 0 samples
and no index. Only pointers, ints and doubles cross the boundary, and nothing
calls back, so a `ctypes` caller needs no structures and the driver's thread
is the only thread that ever runs engine code.

The library: `klattschNative.dll`, x64, 319 KB, built from the same sources
as the static library with the static C runtime. `dumpbin /exports` lists 20
functions and every one is `kl_api_*`. `dumpbin /dependents` lists
`KERNEL32.dll` and nothing else, so no Visual C++ redistributable is needed.
Loaded from 64-bit Python 3 with `ctypes`, it rendered "Hello from the
add-on's library." (69,348 samples at 22,050 Hz) and returned the index
after it at sample 69,348, the last one. On Linux the same target builds
`klattschNative.so`, which is what CI will test the driver against.

## 21.2 The stream, and why it is the same samples

`kl_render_mix` renders each voice section whole, one after another, into a
zeroed float buffer, adding each in. `kl_stream` gives every section its own
`kl_synth`, advances them together, and for each chunk zeroes the output and
adds the sections in the same order. Three details carry the equality:

- **The synth's state survives a call boundary.** Proved at stage 3
  (`stage3-chunked`), so a voice rendered in pieces is the voice rendered
  whole.
- **Float addition in the same order.** The sum is not associative.
  Reversing the section order is one of the mutations, and is caught.
- **A section past its own length is skipped, not added as zeros.**
  `0.0f + -0.0f` is `+0.0f`, so adding zeros could change a sign bit that
  `kl_render_mix` leaves alone. The mutation that renders a section on past
  its end is *not* caught: after its last event every amplitude is 0, so the
  synth produces exact zeros. The skip is kept because it is what
  `kl_render_mix` does, not because a test can see it (§21.5).

Every section is live at once, so each needs its own slice of the
sample-domain schedule, where `kl_render_mix` reuses one array voice after
voice. Sharing one slice is a mutation, and is caught.

**Measured:** `kl_api_test stream` renders every case of
`goldens/cases-compile.bin` (with its bank, engine and seeded extras) both
ways and compares the floats by their bits, at 22,050 Hz with chunks of 1,
137 and 4,096 samples and at 48,000 and 8,000 Hz with chunks of 137. 3,645
renders, 33,572,040 samples, 0 differing, on all four toolchains. Because
`kl_render_mix` is what the stage 6 WAV goldens hold to the JavaScript, this
makes the stream equal to the JavaScript too, by transitivity rather than by
a second comparison.

## 21.3 Where an index falls

NVDA's speech arrives as text with `IndexCommand`s between the pieces. The
API joins the pieces' klattsch source with `\n` and records each index at
the offset where the pieces before it end. The tokenizer gives every token
its offsets in UTF-16 units of the *normalized* source, so that is the unit
the offset is kept in. ASCII is its own normalization, so an ASCII piece
counts its bytes. Any other piece is tokenized on its own to learn its
length.

After compiling, an index fires at the `t_end_ms` of the last phrase whose
`src_end` is at or before its offset, converted to a sample with
`kl_render_samples_for` (ceil, as every sample count in the port is), and
capped at the end. No phrase before it means sample 0. The compiler extends
the final phrase to the end of the trail-off, so an index after the last
word fires when the audio ends.

Why `\n`: it is whitespace to the tokenizer, like a space, and it also ends a
`#` comment. Joined by spaces, a raw-source piece ending in a comment would
swallow every piece after it. That is a mutation, and is caught.

**Worked by hand, and in the test.** Unstressed phonemes at the default rate
take 110 ms each. "HH AH L OW" then an index: the fourth phoneme ends at
440 ms, so the index fires at ⌈440 × 22,050 / 1,000⌉ = **9,702**. "AH" then
an index: 110 ms is 2,425.5 samples, so the index fires at **2,426**. That
second case exists because 9,702 is a whole number, and there a floor and a
ceil give the same answer. It was added before the mutation suite was
written, on noticing that, and it is the check that catches the floor
mutation.

**Homoglyphs.** The same piece written with five Cyrillic look-alikes
(Н Н А Н О: two bytes each, one unit after normalization) must place the
index at 9,702 as well. The first version of this check had one homoglyph,
and the "count bytes, not units" mutation survived it. The one extra byte
lands in the separator before the next piece, and moves nothing. Five
extra bytes reach past the next phoneme.

**A known limit, kept.** Each text piece goes through its own
`kl_text_to_source` call, so the sentence contour restarts at an index in
the middle of a sentence. Say-all puts indexes at line and sentence ends,
so this is rarely heard. If listening says otherwise, the fix is a marker
the front end passes through.

## 21.4 Level

The WAV path normalizes every file to a peak of 0.95, and the generator
plays that file. A stream cannot know its peak before its end, so the API's
16-bit output is the float mix × a fixed output gain × the volume setting,
clamped to ±1 and rounded the way `kl_wav` rounds. The fixed gain was
measured, not chosen: `kl_api_test level` renders each line of the text
corpus and reports what the generator's normalization would have done to it.

| 32 corpus lines, 22,050 Hz, 2026-10-28 | min | p10 | median | p90 | max |
|---|---|---|---|---|---|
| peak of the float mix | 0.770 | 0.817 | 0.865 | 0.878 | 0.896 |
| generator's gain, 0.95 / peak | 1.061 | 1.083 | 1.098 | 1.163 | 1.234 |
| … in dB | +0.51 | +0.69 | +0.81 | +1.31 | +1.83 |

**`KL_API_OUTPUT_GAIN` is 1.0.** For a typical line the generator plays
0.8 dB louder than the add-on will, which is below what a listener notices
between two utterances heard apart. And 1.0 cannot clip: the engine's soft
clip keeps every sample below 1 in magnitude, so the clamp is never
reached. A gain of 1.1 would match the median exactly, and would make the
clamp reachable for any utterance peaking above 0.91. The median is not
worth that.

The other four corpus lines have no peak to measure, because none of them
makes a sound. Three of them do render audio, all of it silence: the front
end writes `,,,` as `p200 p200 p200`, `.` as `.` and `...` as `. . .`, which
are pauses of 600, 300 and 900 ms, each followed by the trail-off. Only
`` ( ) [ ] { } < > / \ | ~ ` ^ `` comes out as nothing. With nothing to hear and
no pause to wait for, `kl_api_end` returns 0 and rendering yields only the
indexes. It does not render the compiler's 250 ms of fade and trail-off, so
an index on its own costs no silence.

*Corrected 2026-10-28.* This paragraph first said that all four lines render
no audio. `level` skips a line whose peak is 0, and that was read as "renders
nothing". The driver test of step 6 fed `...` and got 1.05 s of silence, and
`kl_text_dump --source` on the four lines showed why.

## 21.5 The mutation suite, and the two things it corrected

`tools/api-mutations.py <build>` edits one line, rebuilds, and runs
`kl_api_test stream`, `kl_api_test api` and `tools/verify-text.mjs`. At least
one of them must fail. Final run, MSVC, 2026-10-28: **22 mutations, 18
caught, 4 survive as expected.** Every line is in the script. The four
survivors, and why each is right to survive:

| Mutation | Why nothing can see it |
|---|---|
| a section rendered past its own end | after its last event the section's amplitudes are 0, so the extra samples are exact zeros (§21.2) |
| the silence phoneme `_` counted as sounding | `_` has every amplitude at 0, and the count only decides whether to render at all; a source that is only `_` is not something the front end writes |
| a break of 0 written as `p0` | see below |
| rounding by `floor(x + 0.5)` | it parts from `Math.round` only at 0.49999999999999994 and in the sign of zero, and neither survives conversion to 16 bits. Truncation is the rounding mutation that matters, and it is caught |

**Corrected: "`p0` dips the next 30 ms".** `kl_api_break` skips a break of 0,
and its first comment said that was necessary: `p0` makes the compiler emit
a silence event with a 30 ms transition. The mutation that writes `p0`
anyway was expected to be caught, and was not, so the claim was checked
against the compiler. The silence event always falls on the same sample as
the next event, which is the next phoneme's or, at the latest, the final
fade. The synth drains every event due at a sample before rendering it, and
the later event's targets cover all three amplitudes, so the silence never
sounds. The skip stays, as tidiness. The comment now says so.

**Corrected: the homoglyph check could not fail.** See §21.3. Found because
"count bytes, not units" survived the first run.

## 21.6 What the toolchain matrix found

The first matrix run with this stage in it failed on one leg. Under
clang-cl, `text-frontend` reported 32 failures: every line had lost its
sentence contour.
`b b+9.6 HH EH' L OW(+9.6) …` came out as `b HH EH' L OW …`, and the
generator check failed with it, because it compares against the same tool.
MSVC, both gccs and the API's own checks passed.

The cause was in this stage. `kl_text_dump` filled in its `kl_text_opts`
field by field, `base_f0` and `comma_ms`, the two fields that existed when it
was written. The new `set_inflection` and `inflection` were therefore
whatever was on the stack. MSVC's stack held zeros. clang-cl's did not, so
the contour was scaled to almost nothing. The API's "inflection 1 changes
nothing" check could not see it, because it compares the front end with
itself. The golden comparison is what caught it, which is the reason it
runs on every toolchain.

Fixed both ways. `kl_text_dump` now zeroes the struct. `kl_text.h` now says
that every caller must, because every later field will depend on it. The
generator already did. The second matrix run passed on all four.

## 21.7 Time to first audio, in C

SCREEN-READER.md §3 measured the JavaScript: 1.9 ms to the first 20 ms of a
45-second paragraph, rendering at 42× real time. `kl_api_test bench` measures
the C, through the API, with the whole text corpus as one paragraph (36
lines, 186 s of audio). It times front end, tokenizer, compiler and the
first 20 ms chunk, best of 5, on the development machine, 2026-10-28:

| Rate | First 20 ms after | Whole paragraph |
|---|---|---|
| 22,050 Hz | **1.15 ms** | 535× real time |
| 48,000 Hz | **1.18 ms** | 224× real time |

So time to first audio is about a millisecond for a paragraph four times
longer than the JavaScript's. The figure includes the front end and the
compiler for the whole paragraph; how it divides between them was not
measured. The add-on renders far faster than it plays. Because rendering is
chunked (§3), the time to the first chunk does not grow with the length of
the paragraph.

## 21.8 What changed in the plan

- **NVDA-ADDON.md 5f said** text longer than `KL_TEXT_MAX` must be split at
  sentence ends before the front end. Reading `kl_text_to_source` showed it
  already does: it hands each sentence on at its terminator, and a run with
  none is cut at the buffer and continued, not dropped. Nothing was added.
- **The level** was a measurement still to take; it is §21.4.
- **The `p0` claim** is §21.5.

## 21.9 Step log

- `fa96e34` the API, `kl_stream`, the harness, the shared library; the
  `kl_text_dump` fix from the first matrix run.
- `6bb5319` four places the harness did not reach: an index at part of a
  sample, comments across pieces, a break of 0, inflection 0 and 2.
- `3ca092f` the mutation suite, and the two corrections of §21.5.
- This chapter; `api-mutations.py` in CI.

Next is step 6, the driver, with the NVDA 2026.1 interface read from its
source first.
