# A pronouncing dictionary for the front end — proposal

> **Built 2026-10-28**, the same day, as [23-dictionary.md](23-dictionary.md):
> the whole dictionary, not only what the rules get wrong (the user's
> decision: "store the entire thing, so it would be there, if necessary").
> The proposal below is kept as it was written; where the build departed
> from it, chapter 23 says so.

This is what a dictionary would be for klattsch, worked out on
2026-10-28 from how four other engines in this tree implement theirs. The
decisions it leaves open are at the end.

## Why

The front end ([19-frontend-text.md](19-frontend-text.md)) has no dictionary.
It has the 1976 NRL letter-to-sound rules, behind 17 whole-word respellings.
Measured against the CMU Pronouncing Dictionary on 26,006 ordinary words (the
Hunspell en_US list), the rules get **36.0 %** of words exactly right, and
**83.1 %** of phonemes. Primary stress is right on **82.4 %**
(`goldens/text/accuracy.json`). So most sentences have at least one wrong
word. NVDA-ADDON.md deferred a dictionary "if the rules prove insufficient
in real use". These numbers say they do, before any listening.

## How the others do it

Read from source, 2026-10-28. The comparison, with the measured storage
costs, is kept in the speech-synthesis study repository. These are the
public engines and what each contributes:

| Engine | What it does that matters here |
|---|---|
| **rsynth** (`dengopaiv/rsynth-nvda`) | CMU compiled in, generated during the build by rsynth's own tool. One numeric byte array plus offsets, because MSVC caps a string literal at 64 KB. 126,052 words, a 2 MB table, a 2.7 MB DLL. Digits are read by the number reader before any lookup. It also documents a trap: CMU's schwa `AH0`, stored as `AH`, plays as the vowel of *strut* |
| **SAM** (`dengopaiv/ihamaics`) | CMU as a sorted blob, binary-searched in place. It found that the whole dictionary as a C array is "tens of megabytes of source and a minute of compiler time". The add-on ships CMU's licence in its NOTICE.md and in the file's header |
| **votraxsc01-speech** (Tamas Geczy) | Generated at build time from a checked-in `cmudict.dict` and its licence. **One character per phone-with-stress** (69 symbols). Reduces only CMU's genuinely reduced vowels (`AH0`, `IH0`/`IY0`, `UW0`) and keeps other unstressed vowels full. Overrides CMU's citation stress on **weak forms** (*but, does, from, must, of, us, what*) |
| **Votraxxion** | The 17 respellings klattsch inherited. A linear scan, which is right for 17 entries and wrong for 100,000 |

All four keep CMU's first pronunciation only, so homographs (*read*, *lead*)
stay unresolved. A dictionary does not fix those.

## What is particular to klattsch

- **The phone set needs no mapping.** CMU's 39 phones are exactly the 39 of
  `klatt1980-en`, checked 2026-10-28. The bank's only extra is the silence
  `_`.
- **Stress does more here than in any engine above.** A `'` lengthens the
  vowel 1.5×, lifts it 8 Hz, and counts in the sentence contour. The decline
  steps between stresses, and the nucleus fall lands on the last stress.
  CMU stresses function words in isolation: *of* AH1 V, *to* T UW1, *for*
  F AO1 R, *was* W AA1 Z, *have* HH AE1 V. Taken as they are, they would put
  the nucleus fall on "of". The front end's 81-word `FUNCTION_WORDS` list
  must stay in charge. Function words take no stress whatever CMU says, which
  is votraxsc01-speech's weak-forms rule at klattsch's scale.
- **One stress per word.** CMU's primary `1` becomes `'`; `2` and `0` are
  dropped, as the stress pass already does.
- **No reduced vowels in the bank.** `klatt1980-en` has no `AX` or `IX`, so
  CMU's schwa plays as `AH`, the trap rsynth documented. The rules already
  do the same today, so a dictionary does not make it worse. Adding reduced
  vowels would change the bank, which is engine data, and is postponed with
  phase 3.
- **Numbers stay first.** The number reader turns digits into words before
  letter-to-sound, and only those words, and words of letters and
  apostrophes, would reach the dictionary. That is the house rule for CMU
  dictionaries, and what every engine above does.

## Proposed design

1. **Data.** `data/cmudict/cmudict.dict` at a pinned `cmusphinx/cmudict`
   commit, checked in with its `LICENSE`. CMU's BSD-style notice goes into
   NOTICE.md and the add-on's readme.
2. **Table.** A generator, `tools/build-dict-c.mjs`, writes
   `csrc/kl_dict_data.c`: keys sorted, one byte per phone (39 phones, plus a
   primary-stress flag), and offsets. It gets a `--check` guard in ctest, like
   the bank and NFKC tables. Whether the generated file is checked in, as
   `kl_banks_data.c` is, or generated at build time, as votraxsc01-speech
   does, is decided by its size.
3. **Store only what the rules get wrong.** A word the rules already
   pronounce and stress right gains nothing from an entry, so leaving it out
   loses nothing. The generator runs the rules over every word and keeps the
   difference. Expected well under CMU's 2 MB of text; to be measured.
4. **Lookup.** In `kl_text.c`, per word, after the number reader and before
   pass 1: a binary search. A hit supplies the phones and the stressed vowel,
   and the stress pass then applies `FUNCTION_WORDS` on top. A miss goes to
   the rules as today.
5. **Switchable**, as rsynth and votraxsc01-speech are: a `kl_text_opts`
   flag. Off reproduces today's front end byte for byte, so the existing
   goldens stay valid for that path, and a new golden set covers the
   dictionary path.
6. **Measured honestly.** Against CMU, words in the dictionary score 100 % by
   construction, so that number means nothing. The chapter would report the
   rules on the words the dictionary does *not* hold, and the change on the
   text corpus line by line.

## Open decisions

- **Everything the rules get wrong, or only everyday vocabulary?** CMU is
  mostly surnames and place names, which a screen reader meets constantly.
  The vocabulary-only cut (Hunspell en_US) would be far smaller, and names
  would fall back to the rules.
- **When:** before or after the NVDA hands-on test of
  [22-addon-driver.md](22-addon-driver.md) §22.6. Neither blocks the other.
  The dictionary changes the front end, so the generator and the add-on
  both hear it.
