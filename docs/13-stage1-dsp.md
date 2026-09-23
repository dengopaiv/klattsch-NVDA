# 13. Stage 1 — `kl_dsp.c`

Stage 1 of the [C rewrite](REWRITE.md): the four DSP primitives translated from
`src/engine/dsp.js`. Branch `stage1-dsp`, code in `csrc/kl_dsp.{h,c}`, verified
by `tools/verify-stage1.mjs` against `csrc/tools/kl_dsp_dump.c`.

The first C in the project, and deliberately the smallest: a biquad, a pulse, a
noise generator and a clipper, about 120 lines. The point of the stage is less
the code than proving the harness works end to end — that a C build can be
compared to the JavaScript at all, and that the comparison can fail.

## 13.1 What was translated

| JS | C | Note |
|---|---|---|
| `BandpassBiquad` class | `kl_biquad` struct + four functions | state is plain scalars; no allocation |
| `glottalPulse` | `kl_glottal_pulse` | |
| `xorshift` | `kl_xorshift` | shifts unsigned, returns signed |
| `softClip` | `kl_soft_clip` | |

Every file carries the MIT notice and `Copyright (c) 2026 Tony Gies`, and names
the JavaScript file it was translated from. A translation of someone's
algorithm is still their algorithm.

## 13.2 The four hazards, and what was done about each

These were written down in [REWRITE.md](REWRITE.md) before the port began. All
four were live.

**The LFSR is signed.** `x >>> 17` in JS is a logical shift on the unsigned
reinterpretation, while `<< 13` and `<< 5` wrap; and the caller divides the
**signed** state by 2³¹. The C does the shifts in `uint32_t` — signed
left-shift overflow is undefined in C, so it has to — and casts back to
`int32_t` to return. Getting this wrong gives noise in [0, 2) instead of
[-1, 1) and a DC offset through every fricative. The mutation
`>>> 17` → `>> 17` is in `tools/golden-mutations.sh` for exactly this reason.

**Everything is double.** JS arithmetic is float64 throughout and only the
store into a `Float32Array` rounds. Nothing in this file is `float`.

**The coefficient cache keys on raw arguments.** `setFreq` returns early when
`(f, bw)` are unchanged **before** clamping, so two different raw frequencies
that clamp to the same value each recompute, and the cache hits only on
identical raw arguments. A port that clamps first is observably different at
the clamp boundaries. The `cache` section of the dump tool probes exactly this
and the verifier asserts the equality relations rather than just the values.

**`Math.PI` is the double nearest π**, which is what `M_PI` names — but `M_PI`
is not standard C, so it is spelled out rather than depending on
`_USE_MATH_DEFINES`.

One more, found while writing rather than in advance: **`_setmode(_O_BINARY)`
on Windows**. Without it the CRT turns every `0x0A` in the binary stream into
`0x0D 0x0A` on the way to stdout, and every digest is wrong for a reason that
looks exactly like a DSP bug.

## 13.3 A gap in stage 0 that stage 1 exposed

`goldens/primitives.json` stores a SHA-256 digest for each primitive. **A
digest can only prove bit-exactness** — it cannot express a tolerance. But the
acceptance criterion is split precisely because `glottalPulse` and the biquad
coefficients call `sin` and `cos`, where V8's fdlibm-derived implementations
and the platform libm are not required to agree in the last bit.

So the digests are the right bar for the LFSR and `softClip`, and the wrong bar
for the other two. Rather than loosen the digests, the verifier splits:

- **Tier 1** (`lfsr`, `softclip`, the cache relations) — SHA-256 over the
  canonical byte stream, compared to `primitives.json`. Any difference is a
  bug.
- **Tier 2** (`pulse`, `biquad`) — compared **value by value** against a live
  recomputation from the JS engine, bound 1e-12.

The Tier 2 digests in `primitives.json` keep a job: they are tamper-evidence
that the JavaScript side has not moved. `goldens.mjs --check` proves that, and
the verifier runs it first — because a comparison against a reference that has
silently changed is worse than no comparison.

No golden was loosened or re-captured to make stage 1 pass. The bar moved from
"digest" to "digest for the exact things, values for the inexact ones", which
is what [REWRITE.md](REWRITE.md) said all along; stage 0 simply had not
expressed it yet.

## 13.4 The exit test

> 1,000,000 LFSR states exact; biquad coefficients across the (f, bw) grid and
> the pulse across phase × effort within 1e-12 of the JS.

Result, MSVC 19.51 x64 Release:

```
Tier 1 -- exact, no tolerance
  xorshift 1,000,000 states          ok   3e7273bd9854b289...
  softClip 6001 points               ok   2951b3f2ac2c8aef...
  cache: raw (20,8) != raw (10,5)    ok   both clamp to (40,20), both recompute
  cache: coefficient values          ok   max |diff| 0.00e+0

Tier 2 -- sin/cos involved, bound 1e-12
  glottalPulse 101x1000 grid         ok   max |diff| 2.22e-16 at 62493; 98.74% bit-identical
  biquad coefficient grid            ok   2800 values, all bit-identical
```

✅ Passes on **all four toolchains available here**, across two C runtimes and
two operating systems:

| Toolchain | libm | `glottalPulse` vs the JS |
|---|---|---|
| MSVC 19.51 x64 | UCRT | max \|diff\| 2.22e-16, 98.74% bit-identical |
| clang-cl (VS 18 LLVM) | UCRT | max \|diff\| 2.22e-16, 98.74% bit-identical |
| WinLibs gcc 16.2 (MinGW-w64) | UCRT | max \|diff\| 2.22e-16, 98.74% bit-identical |
| Debian gcc 14.2 under WSL | **glibc 2.41** | max \|diff\| 2.22e-16, 98.40% bit-identical |

`tools/build-matrix.ps1` builds and verifies all four in one command.

### What the numbers say

**The transcendental difference is real but tiny.** `glottalPulse` differs from
the JavaScript on 1.26% of the 101,000 grid points, always by one ULP:
`2.22e-16` at a magnitude near 1. That is ~4 orders of magnitude inside the
1e-12 bound and ~11 orders below the 16-bit LSB (3.05e-5). The split criterion
was the right call, and the bound is not close to binding.

**The biquad coefficients are bit-identical on all 2800 values**, on every
toolchain, including the glibc one. Both libms agree with V8 exactly across
the whole grid. Pleasant, and not something to rely on — the `glottalPulse`
result shows the same functions disagreeing elsewhere — so the tolerance stays.

**The libms genuinely differ, and that is the useful finding.** Cross-comparing
the four builds byte for byte:

| Section | MSVC | clang-cl | WinLibs gcc | WSL gcc |
|---|---|---|---|---|
| `lfsr` | `3E7273BD9854` | `3E7273BD9854` | `3E7273BD9854` | `3E7273BD9854` |
| `softclip` | `2951B3F2AC2C` | `2951B3F2AC2C` | `2951B3F2AC2C` | `2951B3F2AC2C` |
| `pulse` | `5E09C1DFA5B8` | `5E09C1DFA5B8` | `5E09C1DFA5B8` | **`6D2D485FEBFD`** |
| `biquad` | `C11ACBE3797E` | `C11ACBE3797E` | `C11ACBE3797E` | `C11ACBE3797E` |
| `cache` | `1A233902AB05` | `1A233902AB05` | `1A233902AB05` | `1A233902AB05` |

Every Tier 1 section is bit-identical across two libms and two operating
systems — the integer and rational arithmetic is not negotiable and does not
vary. `pulse` differs under glibc, by the same one-ULP magnitude but at a
**different set of points** (98.40% bit-identical against 98.74%, first maximum
at index 60472 rather than 62493).

That is the split acceptance criterion doing exactly what it was written for.
Had the rule been "byte-identical or it is a bug", this stage would have passed
on Windows and failed on Linux, for a reason that is not a bug.

## 13.5 The build

CMake from stage 1, not at the end — the multi-compiler exit test of stage 6
only works if it has been runnable all along. C17, `/W4` on MSVC and
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion` elsewhere; **the translation
compiles clean at those levels on both compilers**. A 32-bit configure is a
hard error rather than a quiet success, per house rule §4.

`ctest` registers three tests: `stage1-dsp` (label `validation`),
`goldens-current` and `banks-current` (label `quick`). All three pass in 8 s.

**The gcc gap is closed.** It was recorded here as unproven, on the grounds
that two Windows compilers sharing a CRT prove less than a Windows and a Linux
compiler. Both legs now exist:

- **WinLibs gcc 16.2** (`C:\GIT\environment\winlibs\mingw64`), a MinGW-w64
  UCRT build. It is deliberately **not on `PATH`** in this environment so it
  cannot shadow MSVC, so the matrix script prepends its directory only for the
  invocation that needs it.
- **Debian gcc 14.2 under WSL**, on glibc 2.41. This is the leg that matters,
  and it earns its place by disagreeing: see the table above. WinLibs being
  UCRT means it is *not* an independent libm from MSVC's, so adding it alone
  would have looked like confirmation while proving very little.

WSL has no node, so its build is verified by dumping the sections to files and
running the verifier from Windows — `tools/verify-stage1.mjs` takes a directory
as well as an executable for exactly this.

## 13.6 Step log

**Translated** `dsp.js` to `csrc/kl_dsp.{h,c}`, C17, no allocation, all
`double`. Clean at `/W4` and at `-Wall -Wextra -Wpedantic -Wshadow
-Wconversion`.

**Dump tool** `csrc/tools/kl_dsp_dump.c` emits the canonical byte stream for
each section rather than computing a hash, so there is no SHA-256 in C to get
wrong and the tolerance comparisons can work on values.

**Stage 0 gap found and handled** — digests cannot express a tolerance; the
verifier splits Tier 1 from Tier 2 (§13.3). No golden loosened.

**Exit test passes** on MSVC and clang-cl, which are byte-identical to each
other. `glottalPulse` max |diff| 2.22e-16 against the JS; biquad bit-identical.

**gcc gap closed** — WinLibs gcc 16.2 (UCRT) and Debian gcc 14.2 under WSL
(glibc 2.41) both build clean and pass. `tools/build-matrix.ps1` runs all four
toolchains in one command; `tools/verify-stage1.mjs` grew a directory mode so a
machine with no node can be verified from one that has it.

**The glibc leg disagreed, as designed.** `pulse` has a different digest under
glibc while every Tier 1 section is bit-identical across all four builds. The
tolerance exists for precisely this, and the measured difference is one ULP.
