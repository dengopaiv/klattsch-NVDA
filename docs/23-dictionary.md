# The pronouncing dictionary — the whole of CMU, compiled in

The front end ([19-frontend-text.md](19-frontend-text.md)) read every word
with the 1976 NRL rules, which get 36 % of ordinary words exactly right. Since
this stage it looks each word up in the CMU Pronouncing Dictionary first, and
the rules handle only what the dictionary lacks. The design, and how four
other engines in this tree built theirs, is in [DICTIONARY.md](DICTIONARY.md).
The decision to **store the entire dictionary** ("so it would be there, if
necessary") is the user's, 2026-10-28. Nothing was filtered out: every
alternative pronunciation and every odd key is kept, though only the first
pronunciation of a plain word is used today.

**Exit test:**

- **The table is the file.** `tools/verify-dict.mjs` (ctest `dict-table`, and
  the matrix) looks up every one of the 135,166 entries through the built
  tool and compares phone for phone ✅. Measured on MSVC, clang-cl, WinLibs
  gcc and WSL Debian gcc (glibc), 2026-10-28. Seven keys not in the file come
  back empty.
- **Off is the old front end.** With the dictionary off, the corpus output is
  byte for byte `goldens/text/source.json` as captured before this stage ✅
  (only that file's `note` line changed). On, it is pinned by the new
  `goldens/text/source-dict.json`.
- **The rules are untouched.** `text-accuracy` (82.41 % stress on vocabulary,
  every count) and the lift digest over 124,076 CMU words are unchanged ✅.
- **The decisions hold.** Twelve hand checks in `verify-dict.mjs` ✅.
- **The checks can fail.** `tools/dict-mutations.py`: 20 mutations, 18
  caught, and the 2 expected to survive did ✅ (§23.6).
- Every ctest entry passes, 18 of 18 on MSVC, with the GUI, API and driver
  tests now running over the dictionary ✅.

---

## 23.1 The data

`data/cmudict/cmudict.dict` and `data/cmudict/LICENSE` are
`cmusphinx/cmudict` at commit `74790861f652b15e4ac49015a90074ad62a27690`,
fetched 2026-10-28. They are checked in byte for byte (`.gitattributes`
pins them `-text`), with SHA-256 `81917843c7f44ce2…` and `bd4ce8e44170a5f9…`.
That is the commit rsynth's build in this tree uses. Its copy, with the line
endings normalised, has the same hash.

| | |
|---|---|
| lines with an entry | 135,166 |
| first pronunciations | 126,052 |
| alternatives (`word(2)`) | 9,114 |
| first pronunciations whose key is letters only | 117,493 |
| … with an apostrophe | 7,433 |
| … with anything else (`.`, `-`, `_`, digits) | 1,126 |
| entries with two or more primary stresses | 1,518 (1.2 %) |
| entries with no primary stress | 82 |

The 22 lines that carry a comment (`# place, danish`) have it removed. CMU's
licence is BSD-style, and its notice is in [NOTICE.md](../NOTICE.md). The
add-on's readme credits it, and both packages carry NOTICE.md.

**The phones need no mapping.** CMU's 39 are exactly the 39 of
`klatt1980-en`. Checked by script, 2026-10-28.

## 23.2 The table

`csrc/tools/kl_dict_gen.c` reads the file and writes `kl_dict_data.c` into
the **build tree**, at build time. CMake builds the generator and runs it.
The 3.9 MB generated source is never checked in, and building needs only a C
compiler, no Node.

- **One character per phone-with-stress.** The 24 consonants and the 15
  vowels with each of CMU's three stress digits make 69 symbols
  (`csrc/kl_dict_symbols.c`, shared by the generator and the library). The
  characters run from `#` upward, skipping `?` (the start of a trigraph,
  which C17 still has) and `\`, so no entry needs an escape.
- **Chunks of at most 4,095 bytes.** That is the longest string literal C17
  requires a compiler to accept, so gcc's `-Wpedantic` stays quiet and
  MSVC's 64 KB literal cap is never near. Each entry is one `"KEY CODES\n"`
  line, and no entry is split. An index of `chunk << 12 | offset` locates
  each one.
- **Keys upper-cased**, because the front end works in upper case, and
  sorted by byte, so `kl_dict.c` binary-searches in place. Nothing is
  allocated or copied.
- **The generator refuses** a phone outside the 69, a line with no phones, a
  key repeated after upper-casing, and an entry longer than a chunk. A
  dictionary that changes shape upstream stops the build instead of losing
  entries.

Result: 135,166 entries in 533 chunks, 2,178,266 bytes of entries plus a
540 KB index. `klattschNative.dll` grew from 319 KB to **3.06 MB**. A clean
MSVC build with the dictionary took 7.4 s: generating the table and
compiling it, with the rest of the tree already built.

## 23.3 How the front end uses it

In pass 1 (`kl_text.c`, `to_arpabet`), in this order:

1. Abbreviations are expanded, as before: "Dr" becomes DOCTOR.
2. The 17 respellings are applied **only to words the dictionary lacks**.
   All 17 are in CMU, so with the dictionary on, none applies.
3. At each position, digits go to the number reader and `$` to the money
   reader, as before. So **numbers never reach the dictionary**, which is
   the house rule for CMU dictionaries.
4. At the start of a word, the word is looked up: letters and apostrophes,
   not glued to digits on either side.
   - A word ending in apostrophes is tried whole, then without them: `'yes'`
     finds YES, and `bosses'` finds itself.
   - An apostrophe after a space is an opening quote, so the word after it
     is looked up. An apostrophe after a letter is mid-word, the S of
     JOHN'S, and nothing after it is looked up.
   - A hit writes the phones in the rules' own spelling, so pass 2 reads both
     alike. CMU's schwa `AH0` is written `AX`, which pass 2 turns into the
     bank's `AH` flagged as reduced, exactly what the rules' schwa becomes.
   - A primary-stressed vowel is followed by `1`, a character no rule ever
     writes (checked: none of the 357 rule outputs holds a digit), and pass 2
     turns it into a flag. Secondary stress is dropped, because klattsch has
     one stress per word.
5. A miss goes to the rules, unchanged.

In pass 3, a word with a dictionary stress takes it, **except a function
word**, which takes none, as from the rules. CMU stresses function words as
they sound alone ("of" AH1 V, "was" W AA1 Z, "for" F AO1 R, "you" Y UW1).
Here a stress lengthens the vowel 1.5×, lifts it 8 Hz and places the
contour's nucleus, so CMU's stress would put the sentence's fall on "of".
When CMU gives **two primaries**, the **last** is taken (seven-TEEN, ay-CEE
for "ac"). That is a judgement, not a measurement: in isolation English
stresses these late, which is also what the spelling rules did for
*seventeen*. It is written down so that it can be changed if listening says
otherwise.

**What does not use it:**
- `kl_text_word`, which `measure-text` scores against CMU; a dictionary word
  would score 100 % there by construction.
- `kl_text_nrl`, the lift's comparison with Votraxxion.
- Spelling.
- The number reader's own words.
- Any caller that sets `kl_text_opts.no_dictionary`, as `kl_text_dump --dict
  off` does.

Every entry point sets the flag itself, so a caller's uninitialised context
cannot turn it on or off by accident.

## 23.4 What it changed

Over the hand corpus, 22 of 36 lines changed. Some of what was wrong and is
now CMU's:

| Word | Rules | Dictionary |
|---|---|---|
| hello | HH EH' L OW | HH AH L OW' |
| brown | B R OW N | B R AW N |
| touch | T AW CH | T AH CH |
| pears | P IY R Z | P EH R Z |
| seventy, seventeen | S IY' V EH N T IY … | S EH' V AH N T IY, … T IY' N |
| dialog, window | D IH' AE L AA G, W AY' N D OW | D AY' AH L AO G, W IH' N D OW |
| version | V ER SH AH N | V ER ZH AH N |
| return | R IY T ER' N | R IH T ER' N |
| photograph | F AA' T AA G R AE F | F OW' T AH G R AE F |
| café, naïve, Zoë | K EY' F, N EY' V, Z OW' | K AH F EY', N AY IY' V, Z OW' IY |
| isn't | IH' S N T | IH' Z AH N T |
| and | AE N D | AH N D |

**Coverage**, measured 2026-10-28 by script against the same packages the
accuracy measurement uses:
- Of the 38,783 stems of the Hunspell en_US list (`dictionary-en` 4.0.0),
  **26,006 (67.1 %)** are CMU entries. The other third, mostly rarer words
  ("asphodel", "chenille", "considerateness"), go to the rules, which get
  about a third of words right.
- Of the hand corpus's 159 word tokens, **151 (95.0 %)** are CMU entries.
  The 8 that are not are ordinal endings (`nd`, `rd`, read by the number
  reader), the abbreviations `prof` and `col` (expanded before lookup), and
  two nonsense words.

The corpus was written to reach code paths, not to represent text, so that
figure is a check, not a quality measure.

## 23.5 What the first runs found

- **A real bug.** `'hello'`, quoted, was not found: the lookup refused any
  word that followed an apostrophe, which was meant for the S of JOHN'S but
  also caught an opening quote. Now only an apostrophe after a letter counts
  as mid-word. `'twas` is found as a result (CMU T W AH1 Z).
- **Four wrong expectations, in the test, not the code.**
  - *bosses'* is B AO1 S in CMU, not B AA S.
  - "search." gets the contour's fall written into its token (`ER'-24`),
    which splits a plain substring.
  - The declination step `b-6` is written between phones.
  - "AO' R" in "Of course …" belongs to *course*, a content word, not to
    "for".

  The checks now strip contour tokens and pitch deltas before matching, and
  name the function words' phones whole.
- **A decision.** The first build took the *first* of two primaries and
  said SEV-enteen. The count of such entries (1,518) and the -teen numbers
  among them led to taking the last (§23.3).
- **A check with no teeth.** "Respellings are skipped for words CMU knows" was
  a hand case. It can never fail: each respelling exists to make the rules
  produce CMU's pronunciation, so with or without it the output is the same.
  It was dropped, and the mutation suite lists that mutation as expected to
  survive. "Letters glued to digits" was strengthened, after confirming that
  ABC and X are CMU keys, with `abc123 x86 mp3`.

## 23.6 The mutation suite

`python tools/dict-mutations.py <build>`, MSVC, 2026-10-28. It runs in CI too.

| Area | Mutations | Caught by |
|---|---|---|
| table | the `?` skip in the codes; keys left lower case; alternatives dropped; a shorter key sorting after; the search's upper bound; the index's offset mask | `verify-dict` (all 6) |
| phones | a two-letter consonant as one letter; HH as H | `verify-text`, `verify-dict` |
| stress | no stress marker; secondary taken as primary; the first of two primaries; function words stressed | `verify-dict` ×3, `verify-text` |
| boundaries | an opening quote; the trailing quote; letters glued to digits; off by default; the word measurement or the lift using the dictionary | `verify-dict` ×4, `measure-text`, `verify-text` |

Expected to survive, and did:

- **AH0 written as AH, not AX.** Both become the bank's AH. The reduced flag
  steers only the spelling rules' stress, which a dictionary word does not
  use.
- **Respellings applied to words CMU knows.** §23.5.

## 23.7 Known limits

- **Homographs** get CMU's first pronunciation: *read* is R EH1 D (the past
  tense) and *lead* as CMU lists it first. The alternatives are in the table
  ("so it would be there, if necessary"), but nothing chooses among them yet.
- **Schwa sounds like the vowel of *strut*.** The bank has no reduced vowel,
  so `AH0` plays as AH, as the rules' schwa already did. rsynth documents the
  same effect. Reduced vowels would be a change to the bank, which is engine
  data, and wait with phase 3.
- **The last of two primaries** is a judgement (§23.3).
- **No user dictionary.** NVDA's own speech dictionaries already let a user
  respell a word before it reaches the synthesizer.

## 23.8 Step log

- `c3faf9a` the data, the generator, the table, the lookup, the front end's
  use of it, `verify-dict`, the goldens, the matrix stage.
- This chapter, `dict-mutations.py` and its CI step, the CMU notice in
  NOTICE.md and the add-on's readme, and `abc123 x86 mp3` in the hand cases.
