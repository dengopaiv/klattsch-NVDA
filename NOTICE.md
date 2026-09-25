# Notices and attributions

Who made what in this repository, and under what terms.

## Tony Gies — klattsch

klattsch is Tony Gies's work: the synthesizer, the phoneme-string language,
the compiler, the phoneme banks as assembled, the web page, and the
JavaScript engine kept here as the frozen reference. MIT, copyright (c) 2026
Tony Gies — the `LICENSE` file, which stays as it is.

The C engine in `csrc/` (every file except the two named below), `bin/` and
the tools that verify it are a translation of that work, and each file says
which JavaScript file it translates. A translation of someone's algorithm is
still their algorithm: those files carry his notice, and so does everything
built from them — including the `ISFT` field the engine writes into every WAV
file.

## The text front end — `csrc/kl_text.c`, `csrc/kl_text_rules.c`

Not part of klattsch, which has no text front end. These two files, their
headers and `csrc/tools/kl_text_dump.c` are BSD-3-Clause, and their history
is recorded in [docs/19-frontend-text.md](docs/19-frontend-text.md). They
come from Votraxxion (`github.com/dengopaiv/Votraxxion`, `src/ttv.c` and
`src/ttv_tables.c`, stage one only), and bring these pieces with them:

- **The letter-to-sound rules**: Elovitz, Johnson, McHugh and Shore,
  *Automatic Translation of English Text to Phonetics by Means of
  Letter-to-Sound Rules*, NRL Report 7948, Naval Research Laboratory, 1976.
  A US Government work.
- **Their arrangement, the number reader's shape and the ASCII character
  names**: John A. Wasser, `english.c`, `saynum.c`, `spellword.c` and
  `parse.c` (1985), released into the public domain.
- **The exception dictionary** (`KL_LTS_EXCEPTIONS`, 17 respellings): Tamas
  Geczy, `exceptions.c` in his votraxsc01 NVDA add-on
  (`github.com/tgeczy/votraxsc01-nvda`). BSD-3-Clause, copyright (c) 2026
  Tamas Geczy. Three of the letter-name corrections in `KL_LTS_ASCII_NAMES`
  are also his.
- **The C matcher, number and money reader, and corrections**, and the new
  passes here — symbols, stress, contour, normalization: Päiv Dengo.
  BSD-3-Clause, copyright (c) 2026 Päiv Dengo.

The terms, as they reach this repository:

```
Copyright (c) 2026, Tamas Geczy
Copyright (c) 2026, Päiv Dengo
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of vsim nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

The third clause is Votraxxion's licence as it stands, which names "vsim"
from the MAME lineage of that repository; it is reproduced as received rather
than edited.

**For the add-on and the generator:** both link the front end, so both are
binary redistributions of it. Their packages must carry this file (or the
two notices above) alongside klattsch's `LICENSE` — MIT for the engine,
BSD-3-Clause for the front end.

## Measured against, not shipped

The front end's accuracy is measured against two word lists that are
installed only for the measurement, never checked in and never packaged:

- the **CMU Pronouncing Dictionary** (Carnegie Mellon University), through
  the `cmu-pronouncing-dictionary` npm package (Zeke Sikelianos, ISC) — the
  same optional dependency upstream's `pronounce.js` names;
- the **Hunspell en_US word list** from SCOWL (Kevin Atkinson et al.),
  through the `dictionary-en` npm package (Titus Wormer; MIT and BSD).

What is checked in from them is counts and a SHA-256 digest
(`goldens/text/accuracy.json`, `goldens/text/nrl.json`), which carry none of
their content.
