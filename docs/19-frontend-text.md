# The text front end — text in, klattsch source out

Steps 2, 3 and 4 of [NVDA-ADDON.md](NVDA-ADDON.md): letter to sound with
stress, normalization, and a sentence contour. Two C files in the library,
`csrc/kl_text.c` and `csrc/kl_text_rules.c`, and one entry point,
`kl_text_to_source()`.

This is not a translation of klattsch. Upstream has no text front end — its
`pronounce.js` is a lookup in an optional npm dictionary with no fallback —
so nothing here is Tony Gies's code, and the provenance is different from
every earlier chapter's. The letter-to-sound pass comes from Votraxxion's
`src/ttv.c`, which is BSD-3-Clause; the rest is new. [NOTICE.md](../NOTICE.md)
names every piece and its terms.

**Exit test:** `tools/verify-text.mjs` (ctest `text-frontend`) and
`tools/measure-text.mjs --check` (ctest `text-accuracy`), both passing on
MSVC, clang-cl and WinLibs gcc, with WSL Debian gcc's output byte-identical to
WinLibs gcc's; and `tools/text-mutations.py` catching all 44 mutations it is
meant to, and not the one equivalent mutant. ✅ Passes. The numbers are in §19.8 and §19.9.

---

## 19.1 The shape, and why the output is source

```
  UTF-8 text
      |  fold: curly quotes, dashes, Latin-1 accents -> ASCII        (§19.6)
      |  split into sentences; expand "Dr." "e.g." ...               (§19.6)
      v
  pass 1   letter to sound  -- NRL rules, numbers, money            (§19.2)
  pass 2   symbols          -- onto the klatt1980-en bank           (§19.3)
  pass 3   stress           -- one per content word                 (§19.4)
  pass 4   contour          -- statement / question / exclamation   (§19.5)
      v
  "b b+9.6 HH EH' L OW(+9.6) , W b-12 ER'-24 L D ."
```

The output is ordinary klattsch source, the same text a person would type into
the web page. Nothing new was added to the grammar and nothing in the engine
changed: stress is klattsch's own `'` mark (×1.5 duration, +8 Hz), the contour
is its `b` directive and its pitch deltas (`+24` sticky, `(+10)` transient),
and pauses are its `,` `;` `.` tokens. That was the plan in
[NVDA-ADDON.md](NVDA-ADDON.md) — "emits ordinary klattsch source and so is
testable without audio" — and it is what made every check in this chapter a
string comparison.

It also means the user's decision of 2026-09-25 holds by construction: the
synthesizer is shipped as it is, and nothing here can change how a given
phoneme string sounds. The 2,187 stage 6 files are untouched, and ctest's
`stage6-wav` still passes.

`kl_text_to_source()` takes a caller-provided `kl_text_ctx` (about 135 KB, no
allocation, nothing static and mutable), the text, an optional base F0, and an
output buffer. It returns the full length of the result even when the buffer
is too short, and cuts a short result at a token boundary — §19.7 says how
that was checked. `kl_text_spell()` reads text character by character for
NVDA's character mode, `kl_text_word()` is the one-word form the measurement
uses, and `kl_text_nrl()` exposes pass 1 alone so it can be compared with
Votraxxion.

---

## 19.2 Pass 1: the lift, and the proof it changed nothing

Votraxxion's front end is text → ARPABET → SC-01 phones. klattsch speaks
ARPABET, so only the first stage came across: the matcher, the number and
money reader, the abbreviation and exception rewrites, and the tables. The
second stage — the NRL "IPA to Votrax" map — has no use here and was left
behind with the 64 SC-01 phone names.

Whose each piece is:

| Piece | Author | Terms |
|---|---|---|
| The letter-to-sound rules | Elovitz, Johnson, McHugh & Shore, NRL Report 7948 (1976) | US Government work |
| Their arrangement, the number reader's shape, the ASCII names | John A. Wasser, `english.c`, `saynum.c`, `spellword.c` (1985) | public domain |
| The exception dictionary (17 respellings) | Tamas Geczy, `exceptions.c` in votraxsc01-nvda | BSD-3-Clause |
| The C matcher, number reader, identifier and ordinal handling, and the corrections | Päiv Dengo, in Votraxxion | BSD-3-Clause |

Two changes were made, and only two:

1. **Every ARPABET character records which position of the working text
   produced it** (`arpa_src[]`). Pass 3 needs it to find which phonemes a
   word's suffix produced (§19.4). It is written beside the output and
   changes none of it.
2. **`;` and `:` pass through as `;`.** Votraxxion drops both, because its
   pause map has nothing between a comma and a full stop. klattsch has `;` (200
   ms), and a clause boundary with no pause at all runs two clauses together.
   Two rows in the punctuation group; the comment above them says so.

The claim that nothing else changed is measured, not asserted.
`tools/capture-text-nrl.mjs` builds a driver around Votraxxion's own `ttv.c` —
`#include`d whole, so its static `to_arpabet()` is reachable without editing a
line of it — and records its output in `goldens/text/nrl.json`:

- the **hand corpus** (`tools/text-corpus.txt`, 36 lines chosen to reach
  every path: numbers, money, ordinals, identifiers, abbreviations, the
  exception words, apostrophes, typographic punctuation, empty and symbol-only
  lines, a 46-letter run) — stored whole;
- **every plain word of the CMU Pronouncing Dictionary** (124,076 words) — as
  a SHA-256 over the output, since the dictionary is not ours to check in.

Captured from Votraxxion `5353a6a`. `verify-text.mjs` runs `kl_text_nrl()`
over the same inputs, removes `;` from its own side (and only `;`), and
requires equality: **36 lines exactly, 124,076 words by digest. Equal.**

What pass 1 is worth as letter to sound is its own number, measured the same
way as stress in §19.4 — against the CMU dictionary, stress ignored:

| | all CMU words | vocabulary |
|---|---|---|
| words whose phonemes equal a CMU pronunciation | 30.80 % | 36.04 % |
| phoneme accuracy (1 − edit distance / length) | 79.67 % | 83.14 % |

That is the NRL rules' quality, inherited, and it is the weakest part of the
front end. It is recorded here as the baseline for any later work on it, not
improved in this step.

---

## 19.3 Pass 2: the symbols

The rules write upper-case pairs for two-letter symbols and lower-case letters
for single consonants (`grEYt`). Split and mapped onto the klatt1980-en bank,
which has 39 phonemes:

| Rules | Bank | Why |
|---|---|---|
| `AX` | `AH`, flagged *reduced* | The bank has no schwa — Klatt 1980's table has none either. CMU writes schwa as `AH0`, so `AH` is the nearest symbol that exists, and the flag keeps pass 3 from stressing it. |
| `WH` | `W` | "where", "what": the bank has no voiceless /w/, and most speakers merged the two long ago. |
| `h` `j` `y` | `HH` `JH` `Y` | The rules' spellings. |
| everything else | itself, upper-cased | |

`verify-text.mjs` check 3 is what holds this pass to account: every line of
output is tokenized and compiled by the JavaScript engine, and must produce no
`unknown` token and no warning. A symbol the bank lacks would compile to an
`unknown phoneme` warning and silence.

---

## 19.4 Pass 3: stress, and how the rules were chosen

The rules give no stress, and klattsch without it gives every syllable the same
length and pitch. One primary stress per content word is what pass 3 adds.

### The measurement

`tools/measure-text.mjs` runs `kl_text_word()` over every plain CMU word and
counts, **among words of two or more syllables where our vowel count equals
the dictionary's** (so letter-to-sound errors are out of the way), how often
our stressed vowel is the dictionary's primary stress. It reports two word
sets:

- **all** 124,076 CMU words, most of them proper names;
- **vocabulary**: the 26,006 of them that are also lower-case entries of the
  Hunspell en_US list (`dictionary-en`, from SCOWL) — ordinary English, which
  is what a screen reader mostly reads, and the set the rules were tuned on.

And a reference on the same words: **always the first vowel**, which is what
the output would be with no stress rules at all beyond "one per word".

Both packages are optional measurement inputs, not shipped and not checked in;
`goldens/text/accuracy.json` records the counts and the package versions
(`cmu-pronouncing-dictionary` 3.0.0, `dictionary-en` 4.0.0).

### The rules, as they ended up

In order, first answer wins:

1. A **function word** (a closed list of 77 — articles, prepositions,
   auxiliaries, pronouns, conjunctions) takes no stress: they are unstressed in
   running speech. They are also irrelevant to the measurement, which only
   counts words of two syllables or more.
2. **One syllable** takes it.
3. The **stem** is taken: endings that never move stress are stripped, up to
   three deep (`-ness -ment -less -ship -ful -able -ible -ing -ist -ism -ed
   -er -ly -s`), and `-ies`/`-ied` are read back as `-y`.
4. The stem's ending decides where one is known to:
   - **self-stressed** (`-ology -ologist -olog -ography -ometer -esque -ique
     -ette -eer -ese -oon -een -ee`): stress the ending's first vowel;
   - **pre-stressing** (`-tion -sion -cian -tial -ious -ic -ical -ity -ify
     -itive -ative -ular -ium -ior -ient -ience -ia` and a few more): stress
     the vowel just before it;
   - **two before** (`-ate -ize -ise -ary -ory`): stress the vowel two before.

   "Which vowel is before the ending" is where `arpa_src[]` from §19.2 comes
   in: a phoneme belongs to the ending if the rule that wrote it started at or
   after the ending's first letter.
5. A **prefix** that is usually unstressed (`be- de- re- con- com- ex- dis-
   mis- ob- sub- ad- en- em- pre- per- ab- sur- in- im- il- ir- un-`, and
   `a-` before a doubled consonant: `acc- acq- aff- agg- all- ann- app- ass-
   att-`) sends it to the vowel after the prefix.
6. Otherwise, **the antepenult** in a stem of three or more vowels, and **the
   first vowel** in a stem of two.
7. A **schwa never takes it**: the next full vowel does, or failing that the
   previous one.

Number words have no spelling to read, and need none — they are a closed set:
"and" unstressed, the -teens on the teen, everything else on its first vowel.

### How they got there

Every change was made one at a time and measured on both sets. Changes that
lost vocabulary words were dropped; changes that gained vocabulary words and
lost proper names were kept, because vocabulary is what the product reads.

| Step | all | vocabulary |
|---|---|---|
| reference: always the first vowel | 68.39 % | 60.88 % |
| first version: function words, suffix tables, prefixes for two-syllable words only, first vowel otherwise | 74.36 % | 77.74 % |
| `-able` `-ible` neutral | 74.46 % | 77.95 % |
| `-er` neutral | 74.78 % | 78.32 % |
| `-ist` `-ism` neutral | 74.78 % | 78.34 % |
| prefixes apply to words of any length, not only two syllables | 75.88 % | 79.81 % |
| `a-` before a doubled consonant (`acc-` … `att-`) | 76.12 % | 80.48 % |
| `-olog` self-stressed (so `ecologist` is found after `-ist` is stripped) | 76.21 % | 80.67 % |
| `-ative` pre-stressing | 76.21 % | 80.68 % |
| `in- im- il- ir- un-` | 76.85 % | 81.84 % |
| antepenult as the default for three or more vowels | 77.25 % | 82.35 % |
| `-ship` neutral | **77.27 %** | **82.41 %** |

Tried and dropped, with what they measured:

- **Penult** as the default for three or more vowels: 74.77 % / 74.78 % — far
  worse than the antepenult.
- **`pro-`** as a prefix: removing it gained one vocabulary word.
- **`arr-`**: no effect either way.
- **`-ance` `-ant` `-ent` `-ess`** neutral: −9, −5, −24, −8 vocabulary words.
  **`-ence`**: +3, **`-hood`**: 0 — too small to be worth a rule.

Kept although they cost proper names: **`be-`** (removing it gains 0.39 % of
all words — Bennett, Berger — and loses 0.18 % of vocabulary) and **`all-`**
(−40 names, +4 vocabulary words).

### What turned out to be wrong, on the way

The first ablation of the prefix list removed each prefix in turn with a
`sed` pattern that expected a comma and a space after the entry. `"AB"` and
`"SUR"` were the last entries on their lines, the pattern matched nothing, and
both were reported as having **no effect** — so they were dropped. The
vocabulary count then fell by 59 words for no visible reason, which is how the
mistake showed itself. Re-measured with an edit that actually removed them,
`ab-` is worth +47 vocabulary words and `sur-` +12; both are back. An ablation
that measures nothing looks exactly like a rule that does nothing, and the
only defence is to check that the thing being removed was removed.

### The result

**82.41 % of vocabulary words and 77.27 % of all CMU words** get their primary
stress on the dictionary's syllable, against 60.88 % and 68.39 % for "always
the first vowel". The misses that remain are mostly words whose stress is a
matter of etymology the spelling does not show (`bouquet`, `chauffeur`,
`gazebo`), compounds (`himself`, `forgot`), and nouns whose prefix is stressed
(`compact`, `district`, `empress`).

---

## 19.5 Pass 4: the contour

Each sentence starts with a bare `b`, which resets F0 to whatever base the
compiler was given — so the contour cannot drift from one sentence to the
next, and never overrides the pitch the user chose. Then, as fractions of that
base:

| | start | decline over the sentence | nucleus | final vowel |
|---|---|---|---|---|
| statement `.` | +8 % | −10 % | fall −20 % | — |
| exclamation `!` | +15 % | −12 % | fall −25 % | — |
| question `?` | +4 % | −4 % | — | rise +30 % |

- The **nucleus** is the last stressed syllable. Its fall is a *sticky* delta,
  so everything after it stays low: "I SAW it" does not bounce back up on
  "it".
- The **decline** is spread in equal `b-` steps before each stressed syllable
  after the first.
- A comma or semicolon inside a sentence gets a **continuation rise**: a
  transient +8 % on the vowel before it, which tells a listener the sentence
  is not over.
- The sizes scale with the base F0 (`kl_text_opts.base_f0`, default 120 Hz),
  so a higher voice gets the same contour in proportion rather than the same
  number of hertz.

**These numbers are stand-ins, not measurements**, and are marked so in the
source. They were chosen to be clearly audible on klattsch's default 120 Hz
voice and to stay inside an octave. The published models are in the
speech-synthesis literature (Pierrehumbert's target-and-interpolation model;
the declination and final-lowering measurements behind rule systems such as
MITalk's), and fitting against one of them is a later step with its own
measurement. What *is* verified is that the contour is exactly what the table
says: `goldens/text/source.json` holds the output for every corpus line, and
the mutation suite (§19.9) changes each column and requires the change to be
caught.

---

## 19.6 Normalization

What reaches pass 1 has been through three things.

**Folding UTF-8 to ASCII.** NVDA hands over text with typographic punctuation
in it, and the rules read ASCII. Code point by code point: curly quotes to
straight, `…` to `.`, an em dash to `,` (it is a clause break when spoken), an
en dash and hyphens to `-`, the no-break and thin spaces to a space, and the
letters of Latin-1 to their base letters — "café" reads as "cafe", which is
how an English reader says it. Anything else outside ASCII becomes a space, so
it ends a word instead of gluing two together.

**Sentences.** Split at `.` `?` `!` followed by whitespace or the end of the
text — the whitespace requirement is what keeps "3.14" whole — unless the
stop belongs to one of the abbreviations below.

**Abbreviations with a full stop**, expanded before the rules see the
sentence: `Mr.` `Mrs.` `Ms.` `Dr.` `Prof.` `Jr.` `Sr.` `vs.` `e.g.` `i.e.`
`etc.`. Without this, "Dr. Smith" is two sentences, the first a lone falling
"D R". `etc.` keeps its stop, because it usually ends a sentence. Votraxxion's
three rewrites (`DR` `MR` `MRS` without a stop) still apply inside pass 1.

The numbers, money, ordinals and identifiers of pass 1 (§19.2) are the rest of
step 3 of the add-on plan, and came across with the lift.

Deliberately **not** done, and why:

- **`St.`, `No.`, `Col.`** and the like: saint or street, number or no — the
  wrong guess is worse than the letters. "Col." in the corpus shows the
  consequence: a sentence break.
- **Symbol names** (`&` `%` `@` `#` …): NVDA's own symbol processing replaces
  them according to the user's punctuation level before the text arrives. A
  synthesizer that named them too would name some twice, or name the ones the
  user asked not to hear.
- **Unicode normalization beyond the fold.** Decomposed accents (a letter
  followed by a combining mark) become the letter and a space. NVDA sends
  composed text in practice; stage 4's note on canonical composition
  ([16-stage4-token.md](16-stage4-token.md) §16.1) is the same question from
  the other side, and the answer is still "measure first".

---

## 19.7 The checks

`tools/verify-text.mjs <kl_text_dump>` — ctest `text-frontend`:

1. **The lift**: §19.2. Hand corpus exactly; the CMU words by digest, when
   the dictionary is installed (reported as SKIPPED otherwise, not passed).
2. **The output**: `--source` over the hand corpus and `--spell` over nine
   strings, against `goldens/text/source.json`, exactly. The front end is
   deterministic, so any change to its output is a change to its rules; it is
   re-captured with `--update`, and the diff of that file is the review.
3. **Short buffers**: the corpus rendered into buffers of 0, 1, 2, 5, 16, 40
   and 100 bytes — 252 cuts — each required to return the full length and to
   hold the longest prefix of whole tokens that fits with its terminator.
   Every earlier test used a 512 KB buffer, so nothing had exercised the cut
   until this was added.
4. **It is klattsch**: every output line tokenized and compiled by the
   JavaScript engine with no unknown token and no warning.

`tools/measure-text.mjs <kl_text_dump> --check` — ctest `text-accuracy`: the
§19.4 counts must equal `goldens/text/accuracy.json`, in **either direction**.
A rule change that gains words still fails, and is re-measured on purpose with
`--update`. Exit 77, which ctest reports as skipped, when the dictionary is
not installed.

---

## 19.8 Four toolchains

`tools/build-matrix.ps1` now has a `text` stage: on MSVC, clang-cl and
WinLibs gcc it runs both checks; WSL has no node, so its `kl_text_dump` is run
in all four modes over the verifier's own inputs (`verify-text.mjs
--list-inputs`: the corpus and all 124,076 words) and its output compared byte
for byte with the WinLibs gcc build's.

| Toolchain | verify-text | text-accuracy | Warnings |
|---|---|---|---|
| MSVC 19.51 (UCRT) | pass | pass | none at /W4 |
| clang-cl (UCRT) | pass | pass | none |
| WinLibs gcc (UCRT) | pass | pass | none at -Wall -Wextra -Wpedantic -Wshadow -Wconversion |
| WSL Debian gcc (glibc) | — | — | output identical to WinLibs gcc in all four modes |

Measured 2026-09-25. The only floating point in the front end is the contour's
rounding to tenths of a hertz (one multiply, one add, one truncation), with no
libm call, so there is no Tier 2 question here; the WSL leg confirms it.

**Speed**, the thing [SCREEN-READER.md](SCREEN-READER.md) §3 cares about:
20,000 lines of a four-sentence, 140-byte paragraph through `--source`, with
nothing else running, in 1.22 s (clang-cl), 1.24 s (WinLibs gcc) and 1.36 s
(MSVC), including process I/O — **about 65 µs a line, 16 µs a sentence**.
Against the 1.9 ms it takes to render the first 20 ms chunk of audio, the
front end is not where latency comes from. (A first timing, taken while the
matrix was rebuilding in the background, gave 1.8–2.5 s; it is not the
number.)

### What the matrix caught

The first full matrix run failed stage 1 on all four toolchains, with
nothing in stage 1 changed. The failing check was stage 1's precondition,
`goldens.mjs --check`, and the cause was this work: the text goldens had been
written as `goldens/text-*.json`, and `goldens.mjs` requires the top level of
`goldens/` to hold exactly the files it writes — worse, when it regenerates
them it **deletes every top-level `*.json` there first**, so the next routine
re-capture of the engine goldens would have silently removed the text ones.
The ctest run on the one toolchain used during development had only been
filtered to the `text` tests, so it had not shown. They now live in
`goldens/text/`, which `goldens.mjs` neither lists nor deletes; stage 1
passes on all four, and the full ctest run (13 entries) passes.

---

## 19.9 The mutation suite

Both checks passed on their first run, which is the least reassuring way to
pass. `tools/text-mutations.py <build-dir>` breaks the front end one line at a
time — 45 mutations, one group per pass and one for the plumbing — rebuilds
`kl_text_dump`, and requires `verify-text` or `text-accuracy` to fail. It
needs the dictionary installed, because without it the stress mutations could
only be caught by the hand corpus and the result would say more about the
corpus than about the front end.

### First run: 43 of 45 as expected

- **"A curly apostrophe is a space"** — a real gap in the corpus. Its only
  curly apostrophe was in "’twas", where the apostrophe is silent either way.
  Added "The dog’s bone isn’t John’s.", where `’s` after a voiced consonant is
  /z/ and a bare `s` is /s/; the Votraxxion capture was re-run for the new
  line. Caught since.
- **"Two word breaks in a row are both kept"** — an *equivalent* mutant. A
  break item writes nothing and stress skips it, so a second one changes no
  output; it only spends a slot in the item buffer. It stays in the suite,
  marked as expected not to be caught, so that the day it *is* caught says
  something changed.

### Second run: 45 of 45 as expected

All 44 real mutations are caught, and the equivalent one is not. Which check catches
what is worth reading in the output: the lift mutations are mostly caught by
both; the contour and plumbing ones only by `verify-text` (the dictionary
measures words, not sentences); and five stress mutations — the `con-` prefix,
`-s` not stripped, `-ies` not read as `-y`, the schwa rule, `WH` passed
through — **only by `text-accuracy`**. Without the dictionary those five would
pass, which is why the suite refuses to run without it and why CI installs it.

---

## 19.10 Known limits

- **Letter to sound is NRL's**: 36 % of vocabulary words exactly right, 83 %
  of phonemes (§19.2). Proper names and loan words fare worse. The known
  remedies — a larger exception list, or rules trained from the dictionary —
  are future work with this measurement as their baseline.
- **No duration model.** Every phoneme still gets klattsch's one `rate` slot,
  ×1.5 when stressed. MITalk's duration rules are the published model; adding
  them means per-phoneme durations, which is an engine change (bank schema v2)
  and was deferred with the rest of stage 7 on 2026-09-25.
- **One level of stress.** klattsch has one mark; secondary stress is not
  represented.
- **The contour's numbers are stand-ins** (§19.5).
- **Abbreviations are few on purpose** (§19.6).

---

## 19.11 Step log

| Date | What |
|---|---|
| 2026-09-25 | Lifted Votraxxion `5353a6a` `src/ttv.c` stage one and its tables into `kl_text.c` / `kl_text_rules.c`; added the position record and the `;` `:` rows. |
| 2026-09-25 | Symbols, stress, contour, spelling. First measurement: stress 74.36 % / 77.74 %. |
| 2026-09-25 | Tuned the stress tables one change at a time (§19.4); final 77.27 % / 82.41 %. Found and corrected the no-op ablation of `ab-`/`sur-`. |
| 2026-09-25 | Number-word stress; abbreviations with a stop; UTF-8 fold. |
| 2026-09-25 | Captured the Votraxxion goldens: corpus exact, 124,076 words by digest — equal. |
| 2026-09-25 | Verifier, accuracy check, short-buffer check, ctest entries. |
| 2026-09-25 | Mutation suite: 43 of 45 as expected, then a corpus line for the curly apostrophe and the equivalent mutant marked — 45 of 45. |
| 2026-09-25 | Four toolchains. The first run exposed the text goldens breaking `goldens.mjs --check`; moved to `goldens/text/`. |
| 2026-09-25 | CI installs the two measurement packages and runs the mutation suite. |
