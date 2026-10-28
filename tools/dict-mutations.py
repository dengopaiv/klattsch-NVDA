#!/usr/bin/env python3
"""Exit test for the dictionary stage: prove its checks catch a broken dictionary.

    python tools/dict-mutations.py <build-dir>

tools/verify-dict.mjs passed on all four toolchains, and a check that has
never failed is either a correct dictionary or a check that is not looking.
Breaking it on purpose tells the two apart.

Four kinds of thing to break:

  * the *table* -- the code characters, the generator's key and comment
    handling, the binary search and the index;
  * the *phones* -- how a dictionary word is written into pass 1;
  * the *stress* -- CMU's primary, the last of two, and the function words
    that take none whatever CMU says;
  * the *boundaries* -- where a word starts and ends, digits, apostrophes,
    the respellings, and which entry points use the dictionary at all.

Each mutation rebuilds and runs tools/verify-dict.mjs, tools/verify-text.mjs
(both goldens) and tools/measure-text.mjs --check (the rules-only word path,
which needs cmu-pronouncing-dictionary installed; without it, a mutation only
it can catch is reported missed). At least one must fail. The few expected to
survive say why: they are places where the code is stricter than anything
observable.

Every pattern must occur exactly once in its file. Exit 0: every mutation
went the way it should.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYM = ROOT / "csrc" / "kl_dict_symbols.c"
DICT = ROOT / "csrc" / "kl_dict.c"
GEN = ROOT / "csrc" / "tools" / "kl_dict_gen.c"
TEXT = ROOT / "csrc" / "kl_text.c"

BUILD_TIMEOUT = 600
VERIFY_TIMEOUT = 600

# (label, file, find, replace[, expect_caught])
MUTATIONS = [
    ("the table", None, None, None),
    ("codes: '?' no longer skipped one way", SYM,
     "    if (c >= '?')\n        c++;\n    if (c >= '\\\\')",
     "    if (c >= '\\\\')"),
    ("generator: keys left in lower case", GEN,
     "            if (es[n].key[i] >= 'a' && es[n].key[i] <= 'z')",
     "            if (0)"),
    ("generator: alternatives dropped", GEN,
     "        es[n].key = dup_n(k, kl);",
     "        if (memchr(k, '(', kl)) continue;\n        es[n].key = dup_n(k, kl);"),
    ("search: a shorter key sorts after", DICT,
     "        if (k == ' ')\n            return -1;",
     "        if (k == ' ')\n            return 1;"),
    ("search: the upper bound skips an entry", DICT,
     "            hi = mid;",
     "            hi = mid ? mid - 1 : 0;"),
    ("index: offsets cut to 11 bits", DICT,
     "    return kl_dict_chunks[at >> 12] + (at & 0xFFFu);",
     "    return kl_dict_chunks[at >> 12] + (at & 0x7FFu);"),

    ("the phones", None, None, None),
    ("a two-letter consonant written as one letter", TEXT,
     "        arpa_puts(x, s);   /* CH DH NG SH TH ZH */",
     "        arpa_putc(x, (char)(s[0] - 'A' + 'a'));"),
    ("HH written as H", TEXT,
     "    } else if (strcmp(s, \"HH\") == 0) {\n        arpa_putc(x, 'h');",
     "    } else if (strcmp(s, \"HH\") == 0) {\n        arpa_putc(x, 'n');"),
    # Expected to survive: AX and AH become the same bank symbol, and the
    # reduced flag only steers the spelling rules' stress, which a dictionary
    # word does not use.
    ("AH0 written as AH, not AX", TEXT,
     "            arpa_puts(x, \"AX\");",
     "            arpa_puts(x, \"AH\");",
     False),

    ("the stress", None, None, None),
    ("the primary-stress marker not written", TEXT,
     "        if (s[2] == '1')\n            arpa_putc(x, '1');",
     "        (void)0;"),
    ("secondary stress taken as primary", TEXT,
     "        if (s[2] == '1')\n            arpa_putc(x, '1');",
     "        if (s[2] != '0')\n            arpa_putc(x, '1');"),
    ("the first of two primaries", TEXT,
     "    for (i = b - 1; i >= a; i--) {",
     "    for (i = a; i < b; i++) {"),
    ("function words stressed as CMU has them", TEXT,
     "            if (w && word_is(w, wl, FUNCTION_WORDS, COUNT(FUNCTION_WORDS)))\n                return -1;\n            return i;",
     "            return i;"),

    ("the boundaries", None, None, None),
    ("an opening quote ends the lookup", TEXT,
     "        (prev == '\\'' && pos >= 2 && is_upper(x->text[pos - 2])))",
     "        prev == '\\'')"),
    ("a trailing quote is never tried off", TEXT,
     "        if (codes || x->text[e - 1] != '\\'')",
     "        if (1)"),
    ("letters glued to digits looked up", TEXT,
     "    if (e < n - 1 && is_digit(x->text[e]))\n        return 0;",
     "    (void)0;"),
    # Expected to survive: every respelling exists to make the rules say what
    # CMU says, so applying it first gives the same phones and the same stress.
    ("respellings applied to words CMU knows", TEXT,
     "        if (x->use_dict && in_dictionary(KL_LTS_EXCEPTIONS[i][0]))\n            continue;",
     "        (void)0;",
     False),
    ("the dictionary off by default", TEXT,
     "    x->use_dict = !(opts && opts->no_dictionary);",
     "    x->use_dict = (opts && opts->no_dictionary);"),
    ("the word measurement uses the dictionary", TEXT,
     "        x->use_dict = 0;   /* rules only: see kl_text.h */",
     "        x->use_dict = 1;"),
    ("the lift comparison uses the dictionary", TEXT,
     "    x->use_dict = 0;       /* the rules' own output, for the lift's comparison */",
     "    x->use_dict = 1;"),
]

MUTATIONS = [m if len(m) == 5 else m + (True,) for m in MUTATIONS]
FILES = sorted({m[1] for m in MUTATIONS if m[1] is not None})


def write_lf(path, text):
    """Write without newline translation; see tools/stage6-mutations.py."""
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
        print("usage: python tools/dict-mutations.py <configured-build-dir>", file=sys.stderr)
        return 2

    rel = [str(p.relative_to(ROOT)) for p in FILES]
    rc, _ = run(["git", "diff", "--quiet", "--"] + rel, timeout=60)
    if rc != 0:
        print("refusing to run: one of " + ", ".join(rel)
              + " has uncommitted changes, and this reverts with git", file=sys.stderr)
        return 2

    dump = find_tool(ROOT / build, "kl_text_dump")
    if dump is None:
        print(f"no kl_text_dump in {build}", file=sys.stderr)
        return 2
    checks = [
        ("verify-dict", ["node", "tools/verify-dict.mjs", str(dump)]),
        ("verify-text", ["node", "tools/verify-text.mjs", str(dump)]),
        ("measure-text", ["node", "tools/measure-text.mjs", str(dump), "--check"]),
    ]

    original = {p: p.read_text(encoding="utf-8") for p in FILES}
    passed = failed = 0

    print("Breaking the table, the phones, the stress and the boundaries; "
          "each line must be caught unless it says otherwise.\n")
    try:
        for label, target, find, repl, expect_caught in MUTATIONS:
            if target is None:
                print(label, flush=True)
                continue

            text = original[target]
            if text.count(find) != 1:
                print(f"  {label:<52} SKIP (pattern matches {text.count(find)} times)", flush=True)
                failed += 1
                continue
            write_lf(target, text.replace(find, repl, 1))

            rc, timed_out = run(["cmake", "--build", str(build), "--config", "Release"])
            if timed_out:
                verdict, ok = "HARNESS TIMEOUT (build)", False
            elif rc != 0:
                verdict = "caught (did not build)" if expect_caught else "CAUGHT (did not build)"
                ok = expect_caught
            else:
                caught_by = None
                for name, cmd in checks:
                    rc, timed_out = run(cmd, timeout=VERIFY_TIMEOUT)
                    if timed_out:
                        caught_by = "TIMEOUT"
                        break
                    if rc not in (0, 77):
                        caught_by = name
                        break
                if caught_by == "TIMEOUT":
                    verdict, ok = "HARNESS TIMEOUT (verify)", False
                elif caught_by:
                    verdict = f"caught by {caught_by}" if expect_caught else \
                              f"CAUGHT by {caught_by} -- thought unobservable"
                    ok = expect_caught
                else:
                    verdict = "NOT CAUGHT  <-- gap" if expect_caught else "not caught, as expected"
                    ok = not expect_caught
            passed += ok
            failed += not ok
            print(f"  {label:<52} {verdict}", flush=True)

            write_lf(target, original[target])
    finally:
        for p in FILES:
            write_lf(p, original[p])
        run(["cmake", "--build", str(build), "--config", "Release"])

    print(f"\ncaught {passed}, missed {failed}")
    if failed:
        print("A mutation went the wrong way: see the line marked above.", file=sys.stderr)
        return 1
    print("Every mutation went the way it should.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
