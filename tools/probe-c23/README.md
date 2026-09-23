# C dialect probe

The feature tests behind the C17-not-C23 decision in
[../../docs/REWRITE.md](../../docs/REWRITE.md), "Which C".

Each `.c` file uses exactly one C23 feature and nothing else, so a compile
failure names the missing feature rather than a pile of them. Run them against
whatever compiler you are considering:

```
cl /nologo /std:clatest /c f1_bool.c     # MSVC
gcc -std=c23 -c f1_bool.c                # gcc
```

`f9b_embed.c` needs a `data.bin` of exactly `ABC` beside it:

```
printf 'ABC' > data.bin
```

Result on MSVC 19.51 (VS 18 Community), 2026-09-23: `/std:clatest` passes
typeof, the attributes, binary literals, digit separators and one-argument
static_assert, and fails constexpr, nullptr, the bool keyword and #embed.
There is no `/std:c23`.

Re-run this when the toolchain moves. The decision is a toolchain fact, not a
preference, so it changes when the fact does.

## A probe that was wrong

The first `#embed` test was `#if __has_embed(__FILE__)` with a `return 0` /
`return 1` split. It compiled under every mode and was recorded as a pass. It
proved nothing: an undefined `__has_embed` makes the `#if` false, so the file
compiles either way and the exit status was never checked. `f9b_embed.c`
replaces it and actually uses `#embed`, which fails on MSVC with
`C1021: invalid preprocessor command 'embed'`.

A probe that cannot fail is not a probe. Kept here as the reason the rest of
them are one feature each.
