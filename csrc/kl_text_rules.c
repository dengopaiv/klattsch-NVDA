/* kl_text_rules.c -- the letter-to-sound tables of the text front end.
 *
 * Not a translation of klattsch.  Upstream has no text front end at all; these
 * tables come to this repository from Votraxxion's src/ttv_tables.c
 * (github.com/dengopaiv/Votraxxion, BSD-3-Clause), where they were stage one
 * of a text-to-SC-01 front end.  Only stage one is taken: English spelling to
 * ARPABET.  Stage two, ARPABET to SC-01 phones, has no use here, because
 * klattsch speaks ARPABET.
 *
 * Whose each table is:
 *
 *   KL_LTS_NRL_RULES, KL_LTS_CARDINALS, KL_LTS_ORDINALS, KL_LTS_ASCII_NAMES,
 *   KL_LTS_ABBREVIATIONS and the scale and currency words
 *       The Naval Research Laboratory letter-to-sound rules (Elovitz, Johnson,
 *       McHugh and Shore, NRL Report 7948, 1976; a US Government work) in the
 *       arrangement of John A. Wasser's english.c, saynum.c, spellword.c and
 *       parse.c (1985, public domain), as vendored byte-identical by Tamas
 *       Geczy in his votraxsc01 NVDA add-on and recovered and checked in
 *       Votraxxion.  Seven ASCII_NAMES letters were corrected in Votraxxion;
 *       three of those corrections are Geczy's.
 *   KL_LTS_EXCEPTIONS
 *       Tamas Geczy's exception dictionary (exceptions.c in
 *       github.com/tgeczy/votraxsc01-nvda).  BSD-3-Clause, copyright tgeczy.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tamas Geczy (KL_LTS_EXCEPTIONS)
 * Copyright (c) 2026 Päiv Dengo (the arrangement and corrections, in Votraxxion)
 * See NOTICE.md for the full terms and for how this sits beside klattsch's MIT
 * licence.
 *
 * Rule format, matching the NRL convention:
 *
 *   {left context, match, right context, output}
 *
 * The matcher in kl_text.c walks the text left to right.  At each position it
 * takes the group for the current character and tries each rule in order; the
 * first whose `match` is present at the cursor and whose contexts both hold
 * wins, its `out` is appended, and the cursor advances by strlen(match).
 *
 * Context character classes:
 *
 *   #  one or more vowels        :  zero or more consonants
 *   ^  one consonant             +  a front vowel (E, I or Y)
 *   %  a suffix -- E, ER, ES, ED, ING or ELY (right context only)
 *   .  a voiced consonant (B D V G J L M N R W Z)
 *
 * Any other character matches itself literally; a space matches a word
 * boundary.  Left contexts are written in reverse reading order, so the
 * character nearest the cursor comes last.
 *
 * Output spelling: upper-case pairs are two-letter ARPABET symbols (AX, EH,
 * SH ...), lower-case letters single-letter consonants ("grEYt").  That is the
 * tables' convention, not ours; kl_text.c's symbol pass relies on it.
 */

#include "kl_text_rules.h"

/* NRL Report 7948 maps every punctuation mark to a space, because it was a
 * letter-to-sound algorithm and had no opinions about timing.  Votraxxion
 * found that to be a bug for a synthesizer and passes "," and "." through, with
 * "?" and "!" folded into "." -- they end a sentence and pause like one; their
 * *pitch* comes from the contour in kl_text.c, which reads the terminator from
 * the original text and never saw this table.  klattsch has pause tokens of its
 * own for exactly these marks (`,` 100 ms, `;` 200 ms, `.` 300 ms), so the
 * pass-through lands on them directly.
 *
 * One deliberate divergence from Votraxxion, and the only one in these tables:
 * the last two rows, ";" and ":", are new here.  Votraxxion drops both,
 * because its pause map has nothing between a comma and a full stop; klattsch
 * has `;`, and a clause boundary spoken as no pause at all runs two clauses
 * together.  tools/verify-text.mjs removes these two marks from its own output
 * before comparing it with Votraxxion's, and says so -- see
 * docs/19-frontend-text.md.
 *
 * The hyphen still emits nothing.  A hyphen is far more often inside a word
 * ("well-known") than standing alone as a dash, and a pause in the middle of a
 * compound is worse than no pause at all. */
static const kl_lts_rule NRL_PUNCT[] = {
    { "",        " ",         "",        " " },
    { "",        "-",         "",        "" },
    { ".",       "'S",        "",        "z" },
    { "#:.E",    "'S",        "",        "z" },
    { "#",       "'S",        "",        "z" },
    { "",        "'",         "",        "" },
    { "",        ",",         "",        "," },
    { "",        ".",         "",        "." },
    { "",        "?",         "",        "." },
    { "",        "!",         "",        "." },
    { "",        ";",         "",        ";" },
    { "",        ":",         "",        ";" },
};

static const kl_lts_rule NRL_A[] = {
    { "",        "A",         " ",       "AX" },
    { " ",       "ARE",       " ",       "AAr" },
    { " ",       "AR",        "O",       "AXr" },
    { "",        "AR",        "#",       "EHr" },
    { "^",       "AS",        "#",       "EYs" },
    { "",        "A",         "WA",      "AX" },
    { "",        "AW",        "",        "AO" },
    { " :",      "ANY",       "",        "EHnIY" },
    { "",        "A",         "^+#",     "EY" },
    { "#:",      "ALLY",      "",        "AXlIY" },
    { " ",       "AL",        "#",       "AXl" },
    { "",        "AGAIN",     "",        "AXgEHn" },
    { "#:",      "AG",        "E",       "IHj" },
    { "",        "A",         "^+:#",    "AE" },
    { " :",      "A",         "^+ ",     "EY" },
    { "",        "A",         "^%",      "EY" },
    { " ",       "ARR",       "",        "AXr" },
    { "",        "ARR",       "",        "AEr" },
    { " :",      "AR",        " ",       "AAr" },
    { "",        "AR",        " ",       "ER" },
    { "",        "AR",        "",        "AAr" },
    { "",        "AIR",       "",        "EHr" },
    { "",        "AI",        "",        "EY" },
    { "",        "AY",        "",        "EY" },
    { "",        "AU",        "",        "AO" },
    { "#:",      "AL",        " ",       "AXl" },
    { "#:",      "ALS",       " ",       "AXlz" },
    { "",        "ALK",       "",        "AOk" },
    { "",        "AL",        "^",       "AOl" },
    { " :",      "ABLE",      "",        "EYbAXl" },
    { "",        "ABLE",      "",        "AXbAXl" },
    { "",        "ANG",       "+",       "EYnj" },
    { "",        "A",         "",        "AE" },
};

static const kl_lts_rule NRL_B[] = {
    { " ",       "BE",        "^#",      "bIH" },
    { "",        "BEING",     "",        "bIYIHNG" },
    { " ",       "BOTH",      " ",       "bOWTH" },
    { " ",       "BUS",       "#",       "bIHz" },
    { "",        "BUIL",      "",        "bIHl" },
    { "",        "B",         "",        "b" },
};

static const kl_lts_rule NRL_C[] = {
    { " ",       "CH",        "^",       "k" },
    { "^E",      "CH",        "",        "k" },
    { "",        "CH",        "",        "CH" },
    { " S",      "CI",        "#",       "sAY" },
    { "",        "CI",        "A",       "SH" },
    { "",        "CI",        "O",       "SH" },
    { "",        "CI",        "EN",      "SH" },
    { "",        "C",         "+",       "s" },
    { "",        "CK",        "",        "k" },
    { "",        "COM",       "%",       "kAHm" },
    { "",        "C",         "",        "k" },
};

static const kl_lts_rule NRL_D[] = {
    { "#:",      "DED",       " ",       "dIHd" },
    { ".E",      "D",         " ",       "d" },
    { "#:^E",    "D",         " ",       "t" },
    { " ",       "DE",        "^#",      "dIH" },
    { " ",       "DO",        " ",       "dUW" },
    { " ",       "DOES",      "",        "dAHz" },
    { " ",       "DOING",     "",        "dUWIHNG" },
    { " ",       "DOW",       "",        "dAW" },
    { "",        "DU",        "A",       "jUW" },
    { "",        "D",         "",        "d" },
};

static const kl_lts_rule NRL_E[] = {
    { "#:",      "E",         " ",       "" },
    { "':^",     "E",         " ",       "" },
    { " :",      "E",         " ",       "IY" },
    { "#",       "ED",        " ",       "d" },
    { "#:",      "E",         "D ",      "" },
    { "",        "EV",        "ER",      "EHv" },
    { "",        "E",         "^%",      "IY" },
    { "",        "ERI",       "#",       "IYrIY" },
    { "",        "ERI",       "",        "EHrIH" },
    { "#:",      "ER",        "#",       "ER" },
    { "",        "ER",        "#",       "EHr" },
    { "",        "ER",        "",        "ER" },
    { " ",       "EVEN",      "",        "IYvEHn" },
    { "#:",      "E",         "W",       "" },
    { "T",       "EW",        "",        "UW" },
    { "S",       "EW",        "",        "UW" },
    { "R",       "EW",        "",        "UW" },
    { "D",       "EW",        "",        "UW" },
    { "L",       "EW",        "",        "UW" },
    { "Z",       "EW",        "",        "UW" },
    { "N",       "EW",        "",        "UW" },
    { "J",       "EW",        "",        "UW" },
    { "TH",      "EW",        "",        "UW" },
    { "CH",      "EW",        "",        "UW" },
    { "SH",      "EW",        "",        "UW" },
    { "",        "EW",        "",        "yUW" },
    { "",        "E",         "O",       "IY" },
    { "#:S",     "ES",        " ",       "IHz" },
    { "#:C",     "ES",        " ",       "IHz" },
    { "#:G",     "ES",        " ",       "IHz" },
    { "#:Z",     "ES",        " ",       "IHz" },
    { "#:X",     "ES",        " ",       "IHz" },
    { "#:J",     "ES",        " ",       "IHz" },
    { "#:CH",    "ES",        " ",       "IHz" },
    { "#:SH",    "ES",        " ",       "IHz" },
    { "#:",      "E",         "S ",      "" },
    { "#:",      "ELY",       " ",       "lIY" },
    { "#:",      "EMENT",     "",        "mEHnt" },
    { "",        "EFUL",      "",        "fUHl" },
    { "",        "EE",        "",        "IY" },
    { "",        "EARN",      "",        "ERn" },
    { " ",       "EAR",       "^",       "ER" },
    { "",        "EAD",       "",        "EHd" },
    { "#:",      "EA",        " ",       "IYAX" },
    { "",        "EA",        "SU",      "EH" },
    { "",        "EA",        "",        "IY" },
    { "",        "EIGH",      "",        "EY" },
    { "",        "EI",        "",        "IY" },
    { " ",       "EYE",       "",        "AY" },
    { "",        "EY",        "",        "IY" },
    { "",        "EU",        "",        "yUW" },
    { "",        "E",         "",        "EH" },
};

static const kl_lts_rule NRL_F[] = {
    { "",        "FUL",       "",        "fUHl" },
    { "",        "F",         "",        "f" },
};

static const kl_lts_rule NRL_G[] = {
    { "",        "GIV",       "",        "gIHv" },
    { " ",       "G",         "I^",      "g" },
    { "",        "GE",        "T",       "gEH" },
    { "SU",      "GGES",      "",        "gjEHs" },
    { "",        "GG",        "",        "g" },
    { " B#",     "G",         "",        "g" },
    { "",        "G",         "+",       "j" },
    { "",        "GREAT",     "",        "grEYt" },
    { "#",       "GH",        "",        "" },
    { "",        "G",         "",        "g" },
};

static const kl_lts_rule NRL_H[] = {
    { " ",       "HAV",       "",        "hAEv" },
    { " ",       "HERE",      "",        "hIYr" },
    { " ",       "HOUR",      "",        "AWER" },
    { "",        "HOW",       "",        "hAW" },
    { "",        "H",         "#",       "h" },
    { "",        "H",         "",        "" },
};

static const kl_lts_rule NRL_I[] = {
    { " ",       "IN",        "",        "IHn" },
    { " ",       "I",         " ",       "AY" },
    { "",        "IN",        "D",       "AYn" },
    { "",        "IER",       "",        "IYER" },
    { "#:R",     "IED",       "",        "IYd" },
    { "",        "IED",       " ",       "AYd" },
    { "",        "IEN",       "",        "IYEHn" },
    { "",        "IE",        "T",       "AYEH" },
    { " :",      "I",         "%",       "AY" },
    { "",        "I",         "%",       "IY" },
    { "",        "IE",        "",        "IY" },
    { "",        "I",         "^+:#",    "IH" },
    { "",        "IR",        "#",       "AYr" },
    { "",        "IZ",        "%",       "AYz" },
    { "",        "IS",        "%",       "AYz" },
    { "",        "I",         "D%",      "AY" },
    { "+^",      "I",         "^+",      "IH" },
    { "",        "I",         "T%",      "AY" },
    { "#:^",     "I",         "^+",      "IH" },
    { "",        "I",         "^+",      "AY" },
    { "",        "IR",        "",        "ER" },
    { "",        "IGH",       "",        "AY" },
    { "",        "ILD",       "",        "AYld" },
    { "",        "IGN",       " ",       "AYn" },
    { "",        "IGN",       "^",       "AYn" },
    { "",        "IGN",       "%",       "AYn" },
    { "",        "IQUE",      "",        "IYk" },
    { "",        "I",         "",        "IH" },
};

static const kl_lts_rule NRL_J[] = {
    { "",        "J",         "",        "j" },
};

static const kl_lts_rule NRL_K[] = {
    { " ",       "K",         "N",       "" },
    { "",        "K",         "",        "k" },
};

static const kl_lts_rule NRL_L[] = {
    { "",        "LO",        "C#",      "lOW" },
    { "L",       "L",         "",        "" },
    { "#:^",     "L",         "%",       "AXl" },
    { "",        "LEAD",      "",        "lIYd" },
    { "",        "L",         "",        "l" },
};

static const kl_lts_rule NRL_M[] = {
    { "",        "MOV",       "",        "mUWv" },
    { "",        "M",         "",        "m" },
};

static const kl_lts_rule NRL_N[] = {
    { "E",       "NG",        "+",       "nj" },
    { "",        "NG",        "R",       "NGg" },
    { "",        "NG",        "#",       "NGg" },
    { "",        "NGL",       "%",       "NGgAXl" },
    { "",        "NG",        "",        "NG" },
    { "",        "NK",        "",        "NGk" },
    { " ",       "NOW",       " ",       "nAW" },
    { "",        "N",         "",        "n" },
};

static const kl_lts_rule NRL_O[] = {
    { "",        "OF",        " ",       "AXv" },
    { "",        "OROUGH",    "",        "EROW" },
    { "#:",      "OR",        " ",       "ER" },
    { "#:",      "ORS",       " ",       "ERz" },
    { "",        "OR",        "",        "AOr" },
    { " ",       "ONE",       "",        "wAHn" },
    { "",        "OW",        "",        "OW" },
    { " ",       "OVER",      "",        "OWvER" },
    { "",        "OV",        "",        "AHv" },
    { "",        "O",         "^%",      "OW" },
    { "",        "O",         "^EN",     "OW" },
    { "",        "O",         "^I#",     "OW" },
    { "",        "OL",        "D",       "OWl" },
    { "",        "OUGHT",     "",        "AOt" },
    { "",        "OUGH",      "",        "AHf" },
    { " ",       "OU",        "",        "AW" },
    { "H",       "OU",        "S#",      "AW" },
    { "",        "OUS",       "",        "AXs" },
    { "",        "OUR",       "",        "AOr" },
    { "",        "OULD",      "",        "UHd" },
    { "^",       "OU",        "^L",      "AH" },
    { "",        "OUP",       "",        "UWp" },
    { "",        "OU",        "",        "AW" },
    { "",        "OY",        "",        "OY" },
    { "",        "OING",      "",        "OWIHNG" },
    { "",        "OI",        "",        "OY" },
    { "",        "OOR",       "",        "AOr" },
    { "",        "OOK",       "",        "UHk" },
    { "",        "OOD",       "",        "UHd" },
    { "",        "OO",        "",        "UW" },
    { "",        "O",         "E",       "OW" },
    { "",        "O",         " ",       "OW" },
    { "",        "OA",        "",        "OW" },
    { " ",       "ONLY",      "",        "OWnlIY" },
    { " ",       "ONCE",      "",        "wAHns" },
    { "",        "ON'T",      "",        "OWnt" },
    { "C",       "O",         "N",       "AA" },
    { "",        "O",         "NG",      "AO" },
    { " :^",     "O",         "N",       "AH" },
    { "I",       "ON",        "",        "AXn" },
    { "#:",      "ON",        " ",       "AXn" },
    { "#^",      "ON",        "",        "AXn" },
    { "",        "O",         "ST ",     "OW" },
    { "",        "OF",        "^",       "AOf" },
    { "",        "OTHER",     "",        "AHDHER" },
    { "",        "OSS",       " ",       "AOs" },
    { "#:^",     "OM",        "",        "AHm" },
    { "",        "O",         "",        "AA" },
};

static const kl_lts_rule NRL_P[] = {
    { "",        "PH",        "",        "f" },
    { "",        "PEOP",      "",        "pIYp" },
    { "",        "POW",       "",        "pAW" },
    { "",        "PUT",       " ",       "pUHt" },
    { "",        "P",         "",        "p" },
};

static const kl_lts_rule NRL_Q[] = {
    { "",        "QUAR",      "",        "kwAOr" },
    { "",        "QU",        "",        "kw" },
    { "",        "Q",         "",        "k" },
};

static const kl_lts_rule NRL_R[] = {
    { " ",       "RE",        "^#",      "rIY" },
    { "",        "R",         "",        "r" },
};

static const kl_lts_rule NRL_S[] = {
    { "",        "SH",        "",        "SH" },
    { "#",       "SION",      "",        "ZHAXn" },
    { "",        "SOME",      "",        "sAHm" },
    { "#",       "SUR",       "#",       "ZHER" },
    { "",        "SUR",       "#",       "SHER" },
    { "#",       "SU",        "#",       "ZHUW" },
    { "#",       "SSU",       "#",       "SHUW" },
    { "#",       "SED",       " ",       "zd" },
    { "#",       "S",         "#",       "z" },
    { "",        "SAID",      "",        "sEHd" },
    { "^",       "SION",      "",        "SHAXn" },
    { "",        "S",         "S",       "" },
    { ".",       "S",         " ",       "z" },
    { "#:.E",    "S",         " ",       "z" },
    { "#:^##",   "S",         " ",       "z" },
    { "#:^#",    "S",         " ",       "s" },
    { "U",       "S",         " ",       "s" },
    { " :#",     "S",         " ",       "z" },
    { " ",       "SCH",       "",        "sk" },
    { "",        "S",         "C+",      "" },
    { "#",       "SM",        "",        "zm" },
    { "#",       "SN",        "'",       "zAXn" },
    { "",        "S",         "",        "s" },
};

static const kl_lts_rule NRL_T[] = {
    { " ",       "THE",       " ",       "DHAX" },
    { "",        "TO",        " ",       "tUW" },
    { "",        "THAT",      " ",       "DHAEt" },
    { " ",       "THIS",      " ",       "DHIHs" },
    { " ",       "THEY",      "",        "DHEY" },
    { " ",       "THERE",     "",        "DHEHr" },
    { "",        "THER",      "",        "DHER" },
    { "",        "THEIR",     "",        "DHEHr" },
    { " ",       "THAN",      " ",       "DHAEn" },
    { " ",       "THEM",      " ",       "DHEHm" },
    { "",        "THESE",     " ",       "DHIYz" },
    { " ",       "THEN",      "",        "DHEHn" },
    { "",        "THROUGH",   "",        "THrUW" },
    { "",        "THOSE",     "",        "DHOWz" },
    { "",        "THOUGH",    " ",       "DHOW" },
    { " ",       "THUS",      "",        "DHAHs" },
    { "",        "TH",        "",        "TH" },
    { "#:",      "TED",       " ",       "tIHd" },
    { "S",       "TI",        "#N",      "CH" },
    { "",        "TI",        "O",       "SH" },
    { "",        "TI",        "A",       "SH" },
    { "",        "TIEN",      "",        "SHAXn" },
    { "",        "TUR",       "#",       "CHER" },
    { "",        "TU",        "A",       "CHUW" },
    { " ",       "TWO",       "",        "tUW" },
    { "",        "T",         "",        "t" },
};

static const kl_lts_rule NRL_U[] = {
    { " ",       "UN",        "I",       "yUWn" },
    { " ",       "UN",        "",        "AHn" },
    { " ",       "UPON",      "",        "AXpAOn" },
    { "T",       "UR",        "#",       "UHr" },
    { "S",       "UR",        "#",       "UHr" },
    { "R",       "UR",        "#",       "UHr" },
    { "D",       "UR",        "#",       "UHr" },
    { "L",       "UR",        "#",       "UHr" },
    { "Z",       "UR",        "#",       "UHr" },
    { "N",       "UR",        "#",       "UHr" },
    { "J",       "UR",        "#",       "UHr" },
    { "TH",      "UR",        "#",       "UHr" },
    { "CH",      "UR",        "#",       "UHr" },
    { "SH",      "UR",        "#",       "UHr" },
    { "",        "UR",        "#",       "yUHr" },
    { "",        "UR",        "",        "ER" },
    { "",        "U",         "^ ",      "AH" },
    { "",        "U",         "^^",      "AH" },
    { "",        "UY",        "",        "AY" },
    { " G",      "U",         "#",       "" },
    { "G",       "U",         "%",       "" },
    { "G",       "U",         "#",       "w" },
    { "#N",      "U",         "",        "yUW" },
    { "T",       "U",         "",        "UW" },
    { "S",       "U",         "",        "UW" },
    { "R",       "U",         "",        "UW" },
    { "D",       "U",         "",        "UW" },
    { "L",       "U",         "",        "UW" },
    { "Z",       "U",         "",        "UW" },
    { "N",       "U",         "",        "UW" },
    { "J",       "U",         "",        "UW" },
    { "TH",      "U",         "",        "UW" },
    { "CH",      "U",         "",        "UW" },
    { "SH",      "U",         "",        "UW" },
    { "",        "U",         "",        "yUW" },
};

static const kl_lts_rule NRL_V[] = {
    { "",        "VIEW",      "",        "vyUW" },
    { "",        "V",         "",        "v" },
};

static const kl_lts_rule NRL_W[] = {
    { " ",       "WERE",      "",        "wER" },
    { "",        "WA",        "S",       "wAA" },
    { "",        "WA",        "T",       "wAA" },
    { "",        "WHERE",     "",        "WHEHr" },
    { "",        "WHAT",      "",        "WHAAt" },
    { "",        "WHOL",      "",        "hOWl" },
    { "",        "WHO",       "",        "hUW" },
    { "",        "WH",        "",        "WH" },
    { "",        "WAR",       "",        "wAOr" },
    { "",        "WOR",       "^",       "wER" },
    { "",        "WR",        "",        "r" },
    { "",        "W",         "",        "w" },
};

static const kl_lts_rule NRL_X[] = {
    { "",        "X",         "",        "ks" },
};

static const kl_lts_rule NRL_Y[] = {
    { "",        "YOUNG",     "",        "yAHNG" },
    { " ",       "YOU",       "",        "yUW" },
    { " ",       "YES",       "",        "yEHs" },
    { " ",       "Y",         "",        "y" },
    { "#:^",     "Y",         " ",       "IY" },
    { "#:^",     "Y",         "I",       "IY" },
    { " :",      "Y",         " ",       "AY" },
    { " :",      "Y",         "#",       "AY" },
    { " :",      "Y",         "^+:#",    "IH" },
    { " :",      "Y",         "^#",      "AY" },
    { "",        "Y",         "",        "IH" },
};

static const kl_lts_rule NRL_Z[] = {
    { "",        "Z",         "",        "z" },
};

// Indexed by letter: [0] is the punctuation/space group, [1 + c - 'A'] the
// group for an upper-case letter.

const kl_lts_rule_group KL_LTS_NRL_RULES[27] = {
    { NRL_PUNCT,  sizeof(NRL_PUNCT) / sizeof(kl_lts_rule) },
    { NRL_A,      sizeof(NRL_A) / sizeof(kl_lts_rule) },
    { NRL_B,      sizeof(NRL_B) / sizeof(kl_lts_rule) },
    { NRL_C,      sizeof(NRL_C) / sizeof(kl_lts_rule) },
    { NRL_D,      sizeof(NRL_D) / sizeof(kl_lts_rule) },
    { NRL_E,      sizeof(NRL_E) / sizeof(kl_lts_rule) },
    { NRL_F,      sizeof(NRL_F) / sizeof(kl_lts_rule) },
    { NRL_G,      sizeof(NRL_G) / sizeof(kl_lts_rule) },
    { NRL_H,      sizeof(NRL_H) / sizeof(kl_lts_rule) },
    { NRL_I,      sizeof(NRL_I) / sizeof(kl_lts_rule) },
    { NRL_J,      sizeof(NRL_J) / sizeof(kl_lts_rule) },
    { NRL_K,      sizeof(NRL_K) / sizeof(kl_lts_rule) },
    { NRL_L,      sizeof(NRL_L) / sizeof(kl_lts_rule) },
    { NRL_M,      sizeof(NRL_M) / sizeof(kl_lts_rule) },
    { NRL_N,      sizeof(NRL_N) / sizeof(kl_lts_rule) },
    { NRL_O,      sizeof(NRL_O) / sizeof(kl_lts_rule) },
    { NRL_P,      sizeof(NRL_P) / sizeof(kl_lts_rule) },
    { NRL_Q,      sizeof(NRL_Q) / sizeof(kl_lts_rule) },
    { NRL_R,      sizeof(NRL_R) / sizeof(kl_lts_rule) },
    { NRL_S,      sizeof(NRL_S) / sizeof(kl_lts_rule) },
    { NRL_T,      sizeof(NRL_T) / sizeof(kl_lts_rule) },
    { NRL_U,      sizeof(NRL_U) / sizeof(kl_lts_rule) },
    { NRL_V,      sizeof(NRL_V) / sizeof(kl_lts_rule) },
    { NRL_W,      sizeof(NRL_W) / sizeof(kl_lts_rule) },
    { NRL_X,      sizeof(NRL_X) / sizeof(kl_lts_rule) },
    { NRL_Y,      sizeof(NRL_Y) / sizeof(kl_lts_rule) },
    { NRL_Z,      sizeof(NRL_Z) / sizeof(kl_lts_rule) },
};

// Words the rules get wrong, rewritten before the rules run -- Tamas Geczy's
// measured exception dictionary (exceptions.c in votraxsc01-nvda,
// BSD-3-Clause, copyright tgeczy).  He measured them through the SC-01; every
// one is a spelling the NRL rules misread, so the respelling is as right for
// ARPABET as it was for the chip.  Each pair is {as written, as respelled};
// the text is space-padded so the leading and trailing blanks act as word
// boundaries.
const char *const KL_LTS_EXCEPTIONS[][2] = {
    { " SEARCH ",      " SURCH " },
    { " SEARCHES ",    " SURCHES " },
    { " SEARCHED ",    " SURCHED " },
    { " SEARCHING ",   " SURCHING " },
    { " RESEARCH ",    " RESURCH " },
    { " HEARD ",       " HURD " },
    { " HEARSE ",      " HURSE " },
    { " REHEARSE ",    " REHURSE " },
    { " REHEARSAL ",   " REHURSAL " },
    { " BEAR ",        " BAIR " },
    { " BEARS ",       " BAIRS " },
    { " WEAR ",        " WAIR " },
    { " WEARS ",       " WAIRS " },
    { " SWEAR ",       " SWAIR " },
    { " SWEARS ",      " SWAIRS " },
    { " PEAR ",        " PAIR " },
    { " PEARS ",       " PAIRS " },
};

// Number names, as ARPABET.  [0..19] are zero..nineteen and [20..27] are
// twenty, thirty .. ninety; KL_LTS_ORDINALS has the same shape.
const char *const KL_LTS_CARDINALS[28] = {
    "zIHrOW", "wAHn", "tUW", "THrIY",
    "fOWr", "fAYv", "sIHks", "sEHvAXn",
    "EYt", "nAYn", "tEHn", "IYlEHvAXn",
    "twEHlv", "THERtIYn", "fOWrtIYn", "fIHftIYn",
    "sIHkstIYn", "sEHvEHntIYn", "EYtIYn", "nAYntIYn",
    "twEHntIY", "THERtIY", "fAOrtIY", "fIHftIY",
    "sIHkstIY", "sEHvEHntIY", "EYtIY", "nAYntIY",
};

const char *const KL_LTS_ORDINALS[28] = {
    "zIHrOWEHTH", "fERst", "sEHkAHnd", "THERd",
    "fOWrTH", "fIHfTH", "sIHksTH", "sEHvEHnTH",
    "EYtTH", "nAYnTH", "tEHnTH", "IYlEHvEHnTH",
    "twEHlvTH", "THERtIYnTH", "fAOrtIYnTH", "fIHftIYnTH",
    "sIHkstIYnTH", "sEHvEHntIYnTH", "EYtIYnTH", "nAYntIYnTH",
    "twEHntIYEHTH", "THERtIYEHTH", "fOWrtIYEHTH", "fIHftIYEHTH",
    "sIHkstIYEHTH", "sEHvEHntIYEHTH", "EYtIYEHTH", "nAYntIYEHTH",
};

// Spoken names for the 128 ASCII codes, as ARPABET -- what a screen reader
// says for a lone character.  Index by the character's own code.
const char *const KL_LTS_ASCII_NAMES[128] = {
    /*   0 */ "nUWl",                        /*   1 */ "stAArt AXv hEHdER",
    /*   2 */ "stAArt AXv tEHkst",           /*   3 */ "EHnd AXv tEHkst",
    /*   4 */ "EHnd AXv trAEnsmIHSHAXn",     /*   5 */ "EHnkwAYr",
    /*   6 */ "AEk",                         /*   7 */ "bEHl",
    /*   8 */ "bAEkspEYs",                   /*   9 */ "tAEb",
    /*  10 */ "lIHnIYfIYd",                  /*  11 */ "vERtIHkAXl tAEb",
    /*  12 */ "fAOrmfIYd",                   /*  13 */ "kAErAYj rIYtERn",
    /*  14 */ "SHIHft AWt",                  /*  15 */ "SHIHft IHn",
    /*  16 */ "dIHlIYt",                     /*  17 */ "dIHvIHs kAAntrAAl wAHn",
    /*  18 */ "dIHvIHs kAAntrAAl tUW",       /*  19 */ "dIHvIHs kAAntrAAl THrIY",
    /*  20 */ "dIHvIHs kAAntrAAl fOWr",      /*  21 */ "nAEk",
    /*  22 */ "sIHnk",                       /*  23 */ "EHnd tEHkst blAAk",
    /*  24 */ "kAEnsEHl",                    /*  25 */ "EHnd AXv mEHsIHj",
    /*  26 */ "sUWbstIHtUWt",                /*  27 */ "EHskEYp",
    /*  28 */ "fAYEHld sIYpERAEtER",         /*  29 */ "grUWp sIYpERAEtER",
    /*  30 */ "rIYkAOrd sIYpERAEtER",        /*  31 */ "yUWnIHt sIYpERAEtER",
    /*  32 */ "spEYs",                       /*  33 */ "EHksklAEmEYSHAXn mAArk",
    /*  34 */ "dAHbl kwOWt",                 /*  35 */ "nUWmbER sAYn",
    /*  36 */ "dAAlER sAYn",                 /*  37 */ "pERsEHnt",
    /*  38 */ "AEmpERsAEnd",                 /*  39 */ "kwOWt",
    /*  40 */ "OWpEHn pEHrEHn",              /*  41 */ "klOWz pEHrEHn",
    /*  42 */ "AEstEHrIHsk",                 /*  43 */ "plAHs",
    /*  44 */ "kAAmmAX",                     /*  45 */ "mIHnAHs",
    /*  46 */ "pIYrIYAAd",                   /*  47 */ "slAESH",
    /*  48 */ "zIHrOW",                      /*  49 */ "wAHn",
    /*  50 */ "tUW",                         /*  51 */ "THrIY",
    /*  52 */ "fOWr",                        /*  53 */ "fAYv",
    /*  54 */ "sIHks",                       /*  55 */ "sEHvAXn",
    /*  56 */ "EYt",                         /*  57 */ "nAYn",
    /*  58 */ "kAAlAXn",                     /*  59 */ "sEHmIHkAAlAXn",
    /*  60 */ "lEHs DHAEn",                  /*  61 */ "EHkwAXl sAYn",
    /*  62 */ "grEYtER DHAEn",               /*  63 */ "kwEHsCHAXn mAArk",
    /*  64 */ "AEt sAYn",                    /*  65 */ "EY",
    /*  66 */ "bIY",                         /*  67 */ "sIY",
    /*  68 */ "dIY",                         /*  69 */ "IY",
    /*  70 */ "EHf",                         /*  71 */ "jIY",
    /*  72 */ "EYCH",                        /*  73 */ "AY",
    /*  74 */ "jEY",                         /*  75 */ "kEY",
    /*  76 */ "EHl",                         /*  77 */ "EHm",
    /*  78 */ "EHn",                         /*  79 */ "OW",
    /*  80 */ "pIY",                         /*  81 */ "kyUW",
    /*  82 */ "AAr",                         /*  83 */ "EHs",
    /*  84 */ "tIY",                         /*  85 */ "yUW",
    /*  86 */ "vIY",                         /*  87 */ "dAHblyUW",
    /*  88 */ "EHks",                        /*  89 */ "wAY",
    /*  90 */ "zIY",                         /*  91 */ "lEHft brAEkEHt",
    /*  92 */ "bAEkslAESH",                  /*  93 */ "rAYt brAEkEHt",
    /*  94 */ "kAErEHt",                     /*  95 */ "AHndERskAOr",
    /*  96 */ "AEpAAstrAAfIH",               /*  97 */ "EY",
    /*  98 */ "bIY",                         /*  99 */ "sIY",
    /* 100 */ "dIY",                         /* 101 */ "IY",
    /* 102 */ "EHf",                         /* 103 */ "jIY",
    /* 104 */ "EYCH",                        /* 105 */ "AY",
    /* 106 */ "jEY",                         /* 107 */ "kEY",
    /* 108 */ "EHl",                         /* 109 */ "EHm",
    /* 110 */ "EHn",                         /* 111 */ "OW",
    /* 112 */ "pIY",                         /* 113 */ "kyUW",
    /* 114 */ "AAr",                         /* 115 */ "EHs",
    /* 116 */ "tIY",                         /* 117 */ "yUW",
    /* 118 */ "vIY",                         /* 119 */ "dAHblyUW",
    /* 120 */ "EHks",                        /* 121 */ "wAY",
    /* 122 */ "zIY",                         /* 123 */ "lEHft brEYs",
    /* 124 */ "vERtIHkAXl bAAr",             /* 125 */ "rAYt brEYs",
    /* 126 */ "tAYld",                       /* 127 */ "dEHl",
};

// Abbreviations expanded before anything else runs, as whole words on
// space-padded text.  Short, and the same three every Votrax-era front end
// carried.  " PHD " was also recognised but has no expansion -- it was spelled
// out letter by letter, which is still the right answer.
const char *const KL_LTS_ABBREVIATIONS[][2] = {
    { " DR ",   " DOCTOR " },
    { " MR ",   " MISTER " },
    { " MRS ",  " MISSUS " },
};

// Scale words, spelled as Wasser's 1985 saynum.c spells them (public domain).
// An ordinal appends TH to whichever of these ends the number.
const char *const KL_LTS_HUNDRED = "hAHndrEHd";
const char *const KL_LTS_THOUSAND = "THAWzAEnd";
const char *const KL_LTS_MILLION = "mIHlIYAXn";
const char *const KL_LTS_BILLION = "bIHlIYAXn";

// Words for reading amounts and decimals, as ARPABET: "3.14" is three POINT
// one four, "$4.20" is four DOLLARS AND twenty CENTS.
const char *const KL_LTS_POINT = "pOYnt";
const char *const KL_LTS_DOLLAR = "dAAlER";
const char *const KL_LTS_DOLLARS = "dAAlERz";
const char *const KL_LTS_AND = "AAnd";
const char *const KL_LTS_CENT = "sEHnt";
const char *const KL_LTS_CENTS = "sEHnts";

/* Counts for the tables whose length the matcher needs and C will not tell
 * it.  Defined here rather than in the header so that adding a row to a table
 * cannot leave a stale count behind in a different file. */
const size_t KL_LTS_EXCEPTION_COUNT =
    sizeof KL_LTS_EXCEPTIONS / sizeof KL_LTS_EXCEPTIONS[0];
const size_t KL_LTS_ABBREVIATION_COUNT =
    sizeof KL_LTS_ABBREVIATIONS / sizeof KL_LTS_ABBREVIATIONS[0];
