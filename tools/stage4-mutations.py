#!/usr/bin/env python3
"""Exit test for stage 4: prove the token comparison catches a broken tokenizer.

    python tools/stage4-mutations.py <build-dir>

Stage 4 has three kinds of thing to break, and a suite that only broke one of
them would be reassuring for the wrong reason:

  * the *cascade order* -- `[bank=x]` must be tried before `[key=value]`, and
    the note form before the compact one. Getting the order wrong still
    produces a token, just the wrong kind, which is exactly the failure a
    "does it parse?" test misses.
  * the *shape* of each pattern -- one character class too wide or too narrow.
  * the *arithmetic* -- Number() and noteToHz, the only two places in this
    stage where a value is computed rather than recognised.

The normalization table is generated, so it is not mutated here: the guard
that matters for it is tools/build-norm-c.mjs --check, which is its own ctest
entry. What *is* mutated is the code that reads the table.

Written in Python, with literal replacement and a deadline on every
subprocess, for the reasons docs/15-stage3-synth.md sets out at length: the
shell version of the stage 3 suite reported eight false "NOT CAUGHT" results
that were its own quoting failures, and then hung for forty minutes with a
mutation still applied.

Exit 0 means every mutation was caught.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "csrc" / "kl_token.c"
NORM = ROOT / "csrc" / "kl_norm.c"

BUILD_TIMEOUT = 180
VERIFY_TIMEOUT = 300

# (label, file, find, replace[, expect_caught])
MUTATIONS = [
    ("cascade order -- the failure a 'does it parse?' test misses", None, None, None),
    # [bank=x] is tried before the generic [key=value]. The generic pattern
    # would not match "[bank=foo]" (foo is not a number), so the token would
    # fall through to unknown rather than becoming a bank switch.
    ("bank pattern moved after the generic one", SRC,
     'if (n > 7 && starts_with(p, n, "[bank=") && p[n - 1] == \']\') {',
     'if (0 && n > 7 && starts_with(p, n, "[bank=") && p[n - 1] == \']\') {'),
    ("engine pattern disabled", SRC,
     'if (n > 9 && starts_with(p, n, "[engine=") && p[n - 1] == \']\') {',
     'if (0 && n > 9 && starts_with(p, n, "[engine=") && p[n - 1] == \']\') {'),
    # The note form is tried before the compact form. Disabling it sends
    # "b=C4" to the compact matcher, which rejects it, so it becomes unknown.
    ("note form disabled", SRC,
     "if (n >= 3 && p[0] == 'b') {",
     "if (0 && n >= 3 && p[0] == 'b') {"),

    ("pattern shape -- one character class too wide or too narrow", None, None, None),
    ("directive key class drops the underscore", SRC,
     "static int is_word(uint16_t c)   { return is_digit(c) || is_upper(c) || is_lower(c) || c == '_'; }",
     "static int is_word(uint16_t c)   { return is_digit(c) || is_upper(c) || is_lower(c); }"),
    ("bank name class drops the dot and dash", SRC,
     "static int is_name(uint16_t c)   { return is_word(c) || c == '.' || c == '-'; }",
     "static int is_name(uint16_t c)   { return is_word(c); }"),
    ("bare uppercase bracket accepts lowercase too", SRC,
     "if (n >= 3 && p[0] == '[' && p[n - 1] == ']' && is_upper(p[1])) {",
     "if (n >= 3 && p[0] == '[' && p[n - 1] == ']' && (is_upper(p[1]) || is_lower(p[1]))) {"),
    ("phoneme delta no longer requires a sign", SRC,
     "if (i < n && (p[i] == '+' || p[i] == '-')) i++; else shape_ok = 0;",
     "if (i < n && (p[i] == '+' || p[i] == '-')) i++;"),
    ("relative ignores the equals sign", SRC,
     "t->relative = (uint8_t)(!eq && sign);",
     "t->relative = (uint8_t)sign;"),
    ("bare p is no longer dropped", SRC,
     'if (strcmp(key, "pause") == 0) return 0;',
     'if (0) return 0;'),
    ("stress mark marks the first phoneme, not the last", SRC,
     "for (size_t j = L->n_tokens; j-- > 0; )",
     "for (size_t j = 0; j < L->n_tokens; j++)"),
    ("transient and sticky deltas swapped", SRC,
     "transient = paren;", "transient = !paren;"),

    ("comments and whitespace", None, None, None),
    ("line comment fires anywhere, not only at a boundary", SRC,
     "if (c == '#' && (i == 0 || kl_is_space(s[i - 1]))) {",
     "if (c == '#') {"),
    ("unterminated block comment stops at the last star", SRC,
     "    return len;\n}", "    return len - 1;\n}"),
    ("block comment inside a token ends the token", SRC,
     "if (at_block_start(s, n, i)) { i = find_block_end(s, n, i); continue; }\n            part[part_len++] = s[i++];",
     "if (at_block_start(s, n, i)) { i = find_block_end(s, n, i); break; }\n            part[part_len++] = s[i++];"),
    ("U+1680 is no longer whitespace", NORM,
     "    case 0x0020: case 0x00A0: case 0x1680:",
     "    case 0x0020: case 0x00A0:"),
    ("U+2028 and U+2029 are no longer whitespace", NORM,
     "    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:",
     "    case 0x202F: case 0x205F: case 0x3000:"),

    ("arithmetic -- the only computed values in the stage", None, None, None),
    ("Number(): fractional scale off by one", SRC,
     "if (scale >= 0 && scale <= 22)        v = (double)mant / POW10[scale];",
     "if (scale >= 0 && scale <= 22)        v = (double)mant / POW10[scale == 0 ? 0 : scale - 1];"),
    ("Number(): sign dropped", SRC,
     "return neg ? -v : v;", "return v;"),
    ("noteToHz: A4 is 441 Hz", SRC,
     "return 440.0 * pow(2.0, (midi - 69.0) / 12.0);",
     "return 441.0 * pow(2.0, (midi - 69.0) / 12.0);"),
    ("noteToHz: flat and sharp swapped", SRC,
     "semi += (s[i] == '#') ? 1 : -1;", "semi += (s[i] == '#') ? -1 : 1;"),
    ("noteToHz: octave offset dropped", SRC,
     "double midi = (double)((octave + 1) * 12 + semi);",
     "double midi = (double)(octave * 12 + semi);"),

    ("normalization -- the code that reads the generated table", None, None, None),
    ("zero-width characters are kept", NORM,
     "for (size_t k = 0; k < n; k++) if (!is_zero_width(out[k])) out[w++] = out[k];",
     "for (size_t k = 0; k < n; k++) out[w++] = out[k];"),
    ("homoglyph pass skipped", NORM,
     "if (homoglyph(out[k], &latin)) out[k] = (uint16_t)(unsigned char)latin;",
     "if (0 && homoglyph(out[k], &latin)) out[k] = (uint16_t)(unsigned char)latin;"),
    ("NFKC expansion truncated to one unit", NORM,
     "for (uint16_t k = 0; k < e->len; k++) out[n++] = kl_norm_pool[e->off + k];",
     "for (uint16_t k = 0; k < 1; k++) out[n++] = kl_norm_pool[e->off + k];"),
]

MUTATIONS = [(m + (True,)) if len(m) == 4 else m for m in MUTATIONS]


def run(cmd, timeout=BUILD_TIMEOUT):
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
        return r.returncode, False
    except subprocess.TimeoutExpired:
        return None, True


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build-msvc")
    if not (ROOT / build).is_dir():
        print("usage: python tools/stage4-mutations.py <configured-build-dir>", file=sys.stderr)
        return 2

    for f in (SRC, NORM):
        rc, _ = run(["git", "diff", "--quiet", "--", str(f.relative_to(ROOT))], timeout=60)
        if rc != 0:
            print(f"refusing to run: {f.name} has uncommitted changes, and this reverts with git",
                  file=sys.stderr)
            return 2

    tok = ROOT / build / "kl_token_dump.exe"
    nrm = ROOT / build / "kl_norm_dump.exe"
    if not tok.exists():
        tok = ROOT / build / "kl_token_dump"
        nrm = ROOT / build / "kl_norm_dump"
    if not tok.exists():
        print(f"no kl_token_dump in {build}", file=sys.stderr)
        return 2

    originals = {f: f.read_text(encoding="utf-8") for f in (SRC, NORM)}
    passed = failed = 0

    print("Breaking the tokenizer; each line must be caught.\n")
    try:
        for entry in MUTATIONS:
            label, target, find, repl = entry[0], entry[1], entry[2], entry[3]
            expect_caught = entry[4] if len(entry) > 4 else True
            if target is None:
                print(label, flush=True)
                continue

            text = originals[target]
            if find not in text:
                print(f"  {label:<52} SKIP (pattern not found)", flush=True)
                failed += 1
                continue
            target.write_text(text.replace(find, repl, 1), encoding="utf-8")

            rc, timed_out = run(["cmake", "--build", str(build)])
            if timed_out:
                verdict = "HARNESS TIMEOUT (build)"
                failed += 1
            elif rc != 0:
                verdict = "caught (did not compile)"
                passed += 1
            else:
                rc, timed_out = run(
                    ["node", "tools/verify-stage4.mjs", str(tok), str(nrm)],
                    timeout=VERIFY_TIMEOUT)
                if timed_out:
                    verdict = "HARNESS TIMEOUT (verify)"
                    failed += 1
                elif rc == 0:
                    verdict = "NOT CAUGHT  <-- gap in the corpus" if expect_caught else "not caught, as expected"
                    failed += expect_caught
                    passed += not expect_caught
                else:
                    verdict = "caught" if expect_caught else "CAUGHT -- the unreachable path is reachable now"
                    passed += expect_caught
                    failed += not expect_caught
            print(f"  {label:<52} {verdict}", flush=True)

            target.write_text(originals[target], encoding="utf-8")
            run(["cmake", "--build", str(build)])
    finally:
        for f, text in originals.items():
            f.write_text(text, encoding="utf-8")
        run(["cmake", "--build", str(build)])

    print(f"\ncaught {passed}, missed {failed}")
    if failed:
        print("A mutation slipped through: the corpus does not reach that path.", file=sys.stderr)
        return 1
    print("All mutations caught.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
