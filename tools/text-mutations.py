#!/usr/bin/env python3
"""Exit test for the text front end: prove its checks catch a broken front end.

    python tools/text-mutations.py <build-dir>

tools/verify-text.mjs and tools/measure-text.mjs --check both passed on their
first run. A check that has never been seen to fail is one nobody should
trust, so this breaks the front end on purpose, one line at a time, and
requires one of the two to notice.

Five kinds of thing to break, one group per pass and one for the plumbing:

  * pass 1, the lift -- a rule, a context class, the number reader. The
    Votraxxion comparison is the check that should catch these.
  * pass 2, the symbols -- the three spellings the bank does not have.
  * pass 3, stress -- each table, the stem, the schwa rule, and the one
    addition to the matcher that stress depends on: the source positions.
  * pass 4, the contour -- each column of the table, the continuation rise,
    the terminator.
  * the plumbing -- abbreviations, Unicode folding, sentence splitting, the
    short-buffer cut, spelling.

The dictionary is optional for the checks but not for this suite: without it
the stress mutations can only be caught by the hand corpus, and the result
would say more about the corpus than about the front end. Set KL_CMU_DIR (or
install cmu-pronouncing-dictionary@3.0.0 and dictionary-en@4.0.0 here).

Every pattern must occur exactly once in its file; a pattern that matches
twice edits a place this suite did not name.

Exit 0 means every mutation was caught.
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEXT = ROOT / "csrc" / "kl_text.c"
RULES = ROOT / "csrc" / "kl_text_rules.c"
FILES = [TEXT, RULES]

BUILD_TIMEOUT = 300
VERIFY_TIMEOUT = 300

# (label, file, find, replace[, expect_caught])
MUTATIONS = [
    ("pass 1 -- the lift", None, None, None),
    ("a rule's output: EE reads IH, not IY", RULES,
     '    { "",        "EE",        "",        "IY" },',
     '    { "",        "EE",        "",        "IH" },'),
    ("a rule dropped: -TION's SH", RULES,
     '    { "",        "TI",        "O",       "SH" },\n',
     ''),
    ("the ; row is gone (ours, not Votraxxion's)", RULES,
     '    { "",        ";",         "",        ";" },\n',
     ''),
    ("left context '#' matches one vowel, not a run", TEXT,
     "            while (pos >= 0 && is_vowel(text[pos])) pos--;\n",
     ""),
    ("suffix class '%' loses -ELY", TEXT,
     "                    if (nx != 'Y') return 0;",
     "                    return 0;"),
    ("the voiced class loses Z", TEXT,
     'strchr("BDVGJLMNRWZ", c)',
     'strchr("BDVGJLMNRW", c)'),
    ("no \"and\" before a remainder under 100", TEXT,
     "        if (value < 100)\n            arpa_say(x, KL_LTS_AND);\n    }\n\n    if (value >= 100) {",
     "        if (value < 10)\n            arpa_say(x, KL_LTS_AND);\n    }\n\n    if (value >= 100) {"),
    ("1100..1999 read in thousands", TEXT,
     "    if ((value >= 1000 && value <= 1099) || value >= 2000) {",
     "    if (value >= 1000) {"),
    ("one cent is \"cents\"", TEXT,
     "arpa_say(x, cents == 1 ? KL_LTS_CENT : KL_LTS_CENTS);",
     "arpa_say(x, KL_LTS_CENTS);"),
    ("leading zero no longer an identifier", TEXT,
     "    if (len > KL_NUMBER_DIGITS || (len > 1 && digits[0] == '0'))",
     "    if (len > KL_NUMBER_DIGITS)"),
    ("exceptions not applied", TEXT,
     "        replace_all(x, KL_LTS_EXCEPTIONS[i][0], KL_LTS_EXCEPTIONS[i][1]);",
     "        (void)0;"),

    ("pass 2 -- the symbols", None, None, None),
    ("AX is passed through, not mapped to AH", TEXT,
     '                memcpy(sym, "AH", 3);\n                reduced = 1;',
     '                reduced = 1;'),
    ("WH is passed through, not mapped to W", TEXT,
     '                memcpy(sym, "W", 2);',
     '                (void)0;'),
    ("h is dropped rather than HH", TEXT,
     '        case \'h\': add_item(x, KL_TEXT_ITEM_PHONE, "HH", src); break;',
     '        case \'h\': break;'),
    # Equivalent: a break writes nothing and stress skips it, so a second one
    # changes no output; it only spends an item slot. Kept to say so.
    ("two word breaks in a row are both kept", TEXT,
     "                x->items[x->item_count - 1].kind == KL_TEXT_ITEM_PHONE)\n                add_item(x, KL_TEXT_ITEM_BREAK, NULL, src);",
     "                1)\n                add_item(x, KL_TEXT_ITEM_BREAK, NULL, src);",
     False),

    ("pass 3 -- stress", None, None, None),
    ("positions not recorded (all at 0)", TEXT,
     "        x->arpa_src[x->arpa_len] = x->cur_src;",
     "        x->arpa_src[x->arpa_len] = 0;"),
    ("\"the\" is a content word", TEXT,
     '    "A", "AN", "THE", "AND",',
     '    "A", "AN", "AND",'),
    ("-TION does not pre-stress", TEXT,
     '    "TIONAL", "SIONAL", "TION", "SION",',
     '    "TIONAL", "SIONAL", "SION",'),
    ("-EE is not self-stressed", TEXT,
     '    "ESE", "OON", "EEN", "EE",',
     '    "ESE", "OON", "EEN",'),
    ("-ATE stresses one before, not two", TEXT,
     "            int t = vowel_before(x, v, nv, ws + sl - n, 1);",
     "            int t = vowel_before(x, v, nv, ws + sl - n, 0);"),
    ("the prefix CON- is forgotten", TEXT,
     '    "BE", "DE", "RE", "CON", "COM",',
     '    "BE", "DE", "RE", "COM",'),
    ("-S is not stripped", TEXT,
     '    "ISM", "ED", "ER", "LY", "S",',
     '    "ISM", "ED", "ER", "LY",'),
    ("-IES is not read as -Y", TEXT,
     "            stem[sl - 1] = 'Y';",
     "            stem[sl - 1] = 'I';"),
    ("long words default to the first syllable", TEXT,
     "        if (stem_vowels >= 3)\n            return v[stem_vowels - 3];",
     "        if (stem_vowels >= 3)\n            return v[0];"),
    ("a schwa may take the stress", TEXT,
     "    if (t < 0 || !(x->items[t].flags & F_REDUCED))\n        return t;",
     "    return t;"),
    ("number words: \"and\" is stressed", TEXT,
     '    if (word_spells(x, a, b, "AA N D"))\n        return -1;',
     '    (void)word_spells;'),
    ("number words: -teen on the first syllable", TEXT,
     "        return v[nv - 1];\n    return v[0];",
     "        return v[0];\n    return v[0];"),

    ("pass 4 -- the contour", None, None, None),
    ("statement fall -20% becomes -21%", TEXT,
     "    /* statement   */ { 0.08, 0.10, -0.20, 0.0 },",
     "    /* statement   */ { 0.08, 0.10, -0.21, 0.0 },"),
    ("statement starts at the base, not above it", TEXT,
     "    /* statement   */ { 0.08, 0.10, -0.20, 0.0 },",
     "    /* statement   */ { 0.00, 0.10, -0.20, 0.0 },"),
    ("question rise is transient, not sticky", TEXT,
     "        x->items[last_vowel].flags |= F_STICKY;",
     "        x->items[last_vowel].flags |= F_TRANS;"),
    ("no continuation rise before a semicolon", TEXT,
     "        } else if (it->kind == KL_TEXT_ITEM_PAUSE && it->sym[0] != '.' &&",
     "        } else if (it->kind == KL_TEXT_ITEM_PAUSE && it->sym[0] == ',' &&"),
    ("\"?\" read as a statement", TEXT,
     "    if (terminator == '?') return S_QUESTION;",
     "    if (terminator == '#') return S_QUESTION;"),
    ("no reset at the start of a sentence", TEXT,
     '        sink_token(s, "b");\n        if (start10 != 0) {',
     '        if (start10 != 0) {'),
    ("declination on the first stress too", TEXT,
     "            if ((x->items[i].flags & F_STRESS) && i != first_stress &&",
     "            if ((x->items[i].flags & F_STRESS) &&"),
    ("tenths rounded down, not to nearest", TEXT,
     "    return (int)(t < 0 ? t - 0.5 : t + 0.5);",
     "    return (int)t;"),

    ("the plumbing", None, None, None),
    ("\"Dr.\" is not expanded", TEXT,
     '    { "DR.",   "DOCTOR" },',
     '    { "DQ.",   "DOCTOR" },'),
    ("an abbreviation's stop ends the sentence", TEXT,
     "            if ((p >= end || is_space((char)*p)) &&\n                !abbreviation_ends_at(sent, (int)n))",
     "            if ((p >= end || is_space((char)*p)))"),
    ("\"etc.\" loses its stop", TEXT,
     '    { "ETC.",  "ET CETERA." },',
     '    { "ETC.",  "ET CETERA" },'),
    ("a curly apostrophe is a space", TEXT,
     "    case 0x2018: case 0x2019: case 0x201B: case 0x2032: return \"'\";",
     "    case 0x2018: case 0x201B: case 0x2032: return \"'\";"),
    ("an em dash is not a clause break", TEXT,
     '    case 0x2014: return ",";',
     '    case 0x2014: return " ";'),
    ("accented letters are dropped", TEXT,
     "        return LATIN1[cp - 0xC0];",
     "        return \" \";"),
    ("a sentence ends at a stop inside a number", TEXT,
     "            if ((p >= end || is_space((char)*p)) &&",
     "            if (1 &&"),
    ("a short buffer takes a token that does not fit", TEXT,
     "    if (!s->full && s->out && s->written + need < s->cap) {",
     "    if (!s->full && s->out && s->written + need <= s->cap) {"),
    ("a short buffer resumes after a token that did not fit", TEXT,
     "    } else {\n        s->full = 1;\n    }",
     "    }"),
    ("a comma is the engine's 100 ms again, not the front end's pause", TEXT,
     "    int comma_ms = (opts && opts->comma_ms > 0) ? opts->comma_ms : KL_TEXT_COMMA_MS;",
     "    int comma_ms = (opts && opts->comma_ms > 0) ? opts->comma_ms : 0;"),
    ("spelling: no comma between characters", TEXT,
     "        if (!first)\n            arpa_putc(x, ',');",
     "        (void)first;"),
]


def write_lf(path, text):
    """Write without newline translation (.gitattributes pins *.c to LF)."""
    path.write_bytes(text.encode("utf-8"))


def run(cmd, timeout=BUILD_TIMEOUT):
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
        return r.returncode, False
    except subprocess.TimeoutExpired:
        return None, True


def find_tool(build, name):
    for p in (build / name, build / "Release" / name,
              build / (name + ".exe"), build / "Release" / (name + ".exe")):
        if p.exists():
            return p
    return None


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build")
    if not (ROOT / build).is_dir():
        print("usage: python tools/text-mutations.py <configured-build-dir>", file=sys.stderr)
        return 2

    rel = [str(p.relative_to(ROOT)) for p in FILES]
    rc, _ = run(["git", "diff", "--quiet", "--"] + rel, timeout=60)
    if rc != 0:
        print("refusing to run: one of " + ", ".join(rel)
              + " has uncommitted changes, and a crash here would lose them", file=sys.stderr)
        return 2

    dump = find_tool(ROOT / build, "kl_text_dump")
    if dump is None:
        print(f"no kl_text_dump in {build}", file=sys.stderr)
        return 2

    checks = [["node", "tools/verify-text.mjs", str(dump)],
              ["node", "tools/measure-text.mjs", str(dump), "--check"]]
    rc, _ = run(checks[1], timeout=VERIFY_TIMEOUT)
    if rc == 77:
        print("the CMU dictionary is not installed; see this file's docstring", file=sys.stderr)
        return 2

    original = {p: p.read_text(encoding="utf-8") for p in FILES}
    passed = failed = 0

    print("Breaking the text front end; each line must be caught.\n")
    try:
        for entry in MUTATIONS:
            label, target, find, repl = entry[:4]
            expect_caught = entry[4] if len(entry) > 4 else True
            if target is None:
                print(label, flush=True)
                continue
            text = original[target]
            if text.count(find) != 1:
                print(f"  {label:<58} SKIP (pattern matches {text.count(find)} times)", flush=True)
                failed += 1
                continue
            write_lf(target, text.replace(find, repl, 1))

            rc, timed_out = run(["cmake", "--build", str(build), "--config", "Release",
                                 "--target", "kl_text_dump"])
            if timed_out:
                verdict, ok = "HARNESS TIMEOUT (build)", False
            elif rc != 0:
                verdict, ok = "caught (did not compile)", True
            else:
                caught_by = []
                for name, cmd in zip(("verify", "accuracy"), checks):
                    rc, timed_out = run(cmd, timeout=VERIFY_TIMEOUT)
                    if timed_out:
                        caught_by.append(name + " TIMED OUT")
                    elif rc != 0:
                        caught_by.append(name)
                if any("TIMED OUT" in c for c in caught_by):
                    verdict, ok = "HARNESS TIMEOUT: " + ", ".join(caught_by), False
                elif not caught_by:
                    verdict = "NOT CAUGHT  <-- gap" if expect_caught else "not caught, as expected"
                    ok = not expect_caught
                else:
                    verdict = ("caught by " + " and ".join(caught_by) if expect_caught
                               else "CAUGHT -- the equivalent mutant is not equivalent")
                    ok = expect_caught
            passed += ok
            failed += not ok
            print(f"  {label:<58} {verdict}", flush=True)
            write_lf(target, original[target])
    finally:
        for p, t in original.items():
            write_lf(p, t)
        run(["cmake", "--build", str(build), "--config", "Release", "--target", "kl_text_dump"])

    print(f"\n{passed} caught, {failed} not")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
