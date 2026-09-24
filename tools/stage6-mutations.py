#!/usr/bin/env python3
"""Exit test for stage 6: prove the file comparison catches a broken encoder.

    python tools/stage6-mutations.py <build-dir>

Stage 6 passed its verifier on the first run, as stage 5 did. That is what
this suite is for: a comparison of 2,187 whole files that never fails is
either a correct port or a comparison that is not looking, and only breaking
the port on purpose tells the two apart.

Six kinds of thing to break, because a suite that broke only one of them
would be reassuring for the wrong reason:

  * the *header* -- twelve fields, every one of which is a plausible typo and
    none of which changes how the file sounds in a player that ignores it.
  * the *INFO chunk* -- the order of the two fields, the pad byte that is
    counted in one length and not the other, and the ISFT field itself. That
    last one is not a numeric concern: it is upstream's credit, and a port
    that drops it is wrong in the way this repository cares most about.
  * the *normalization* -- the gain is a function of one sample of the whole
    mix, so everything about how that sample is found matters.
  * the *rounding* -- Math.round is neither round() nor floor(x + 0.5), and
    toFixed is neither of those either. Four mutations here are the C library
    functions a translator would reach for first.
  * the *mix* -- voice order, voice length, add versus assign. Float addition
    is not associative, so the order is observable.
  * the *CLI* -- the sample rate, the warning line, the default output path,
    and the two Windows-only conversions that make a non-ASCII argument or
    output path work.

Every pattern must occur exactly once in its file; a pattern that matches
twice edits a place this suite did not name and reports whatever that does as
the result for this line.

Exit 0 means every mutation was caught.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WAV = ROOT / "csrc" / "kl_wav.c"
WAVH = ROOT / "csrc" / "kl_wav.h"
REN = ROOT / "csrc" / "kl_render.c"
FMT = ROOT / "csrc" / "kl_fmt.c"
CLI = ROOT / "bin" / "klattsch_cli.c"
DUMP = ROOT / "csrc" / "tools" / "kl_wav_dump.c"

WIN = sys.platform == "win32"

BUILD_TIMEOUT = 300
VERIFY_TIMEOUT = 600

# (label, file, find, replace[, expect_caught])
MUTATIONS = [
    ("the RIFF header -- twelve fields nobody listens to", None, None, None),
    ("the RIFF size stops counting the LIST chunk", WAV,
     "    put_u32(out + 4, (uint32_t)(total - 8));",
     "    put_u32(out + 4, (uint32_t)(44 + data_bytes - 8));"),
    ("the byte rate loses its factor of two", WAV,
     "    put_u32(out + 28, sample_rate * 2);              /* byte rate */",
     "    put_u32(out + 28, sample_rate);                  /* byte rate */"),
    ("the block align says one byte per frame", WAV,
     "    put_u16(out + 32, 2);                           /* block align */",
     "    put_u16(out + 32, 1);                           /* block align */"),
    ("the file claims 8-bit samples", WAV,
     "    put_u16(out + 34, 16);                          /* bits per sample */",
     "    put_u16(out + 34, 8);                           /* bits per sample */"),
    ("the file claims to be stereo", WAV,
     "    put_u16(out + 22, 1);                           /* mono */",
     "    put_u16(out + 22, 2);                           /* mono */"),
    ("the data chunk size counts the whole file", WAV,
     "    put_u32(out + 40, (uint32_t)data_bytes);",
     "    put_u32(out + 40, (uint32_t)total);"),
    ("the samples go out big-endian", WAV,
     """        out[o]     = (unsigned char)((uint32_t)v & 0xFFu);
        out[o + 1] = (unsigned char)(((uint32_t)v >> 8) & 0xFFu);""",
     """        out[o]     = (unsigned char)(((uint32_t)v >> 8) & 0xFFu);
        out[o + 1] = (unsigned char)((uint32_t)v & 0xFFu);"""),

    ("the LIST INFO chunk, and the attribution in it", None, None, None),
    ("ICMT is written before ISFT", WAV,
     """        if (sw_len) p += put_sub(p, "ISFT", meta->software, sw_len);
        if (cm_len) p += put_sub(p, "ICMT", meta->comment,  cm_len);""",
     """        if (cm_len) p += put_sub(p, "ICMT", meta->comment,  cm_len);
        if (sw_len) p += put_sub(p, "ISFT", meta->software, sw_len);"""),
    ("the pad byte is counted in the sub-chunk size", WAV,
     "    put_u32(p + 4, (uint32_t)len);",
     "    put_u32(p + 4, (uint32_t)(len + len % 2));"),
    # Not "remove the branch": the padded byte would then be whatever malloc
    # left there, and a mutation whose result depends on the allocator is not
    # a measurement. Writing the wrong value is deterministic and tests the
    # same thing -- that the byte is written, and is zero.
    ("the pad byte is not zero", WAV,
     "        p[8 + len] = 0;", "        p[8 + len] = 1;"),
    ("the LIST payload size forgets the padding", WAV,
     "    if (c) { payload += 8 + c + (c % 2); subs++; }",
     "    if (c) { payload += 8 + c; subs++; }"),
    # Three sites at once, because one is a no-op: an empty string and an
    # absent one both have length 0, so `strlen("")` makes the two spellings
    # of the guard the same function. The mistake a mechanical translation
    # actually makes is to key the field on the *pointer* rather than on the
    # length, consistently, which writes an ISFT sub-chunk of length zero.
    # Measured before this entry existed: it grows the emptySoftware golden
    # from 26,086 bytes to 26,094, and the golden catches it.
    ("an empty software field becomes a present one", WAV,
     ["        if (meta->software && meta->software[0]) s = strlen(meta->software);",
      "    if (s) { payload += 8 + s + (s % 2); subs++; }",
      '        if (sw_len) p += put_sub(p, "ISFT", meta->software, sw_len);'],
     ["        if (meta->software) s = strlen(meta->software);",
      "    if (meta && meta->software) { payload += 8 + s + (s % 2); subs++; }",
      '        if (meta->software) p += put_sub(p, "ISFT", meta->software, sw_len);']),
    # The one this repository cares about most. Two entries, because the file
    # is written by two programs and losing the credit in either is the same
    # loss to the person who ends up with the file.
    ("the ISFT field is dropped from the corpus renders", DUMP,
     "        meta.software = KL_WAV_SOFTWARE;",
     "        meta.software = NULL;"),
    ("the ISFT field is dropped from the CLI", CLI,
     "    meta.software = KL_WAV_SOFTWARE;",
     "    meta.software = NULL;"),
    ("the attribution string itself is altered", WAVH,
     '#define KL_WAV_SOFTWARE "klattsch \\xC2\\xB7 https://tgies.github.io/klattsch"',
     '#define KL_WAV_SOFTWARE "klattsch"'),

    ("normalization -- one sample decides every byte", None, None, None),
    ("the target peak is 0.9 instead of 0.95", WAVH,
     "#define KL_WAV_PEAK_NORMALIZE 0.95",
     "#define KL_WAV_PEAK_NORMALIZE 0.9"),
    ("the peak is the last sample, not the largest", WAV,
     "            if (a > peak) peak = a;", "            peak = a;"),
    ("the peak ignores the sign", WAV,
     "            double a = v < 0.0 ? -v : v;", "            double a = v;"),
    ("a silent buffer divides by zero", WAV,
     "        if (peak > 0.0) gain = peak_normalize / peak;",
     "        if (peak >= 0.0) gain = peak_normalize / peak;"),
    # Unreachable, and measured rather than assumed: kl_wav_gain is called
    # with KL_WAV_PEAK_NORMALIZE or with a literal 0.0, never with NaN. The
    # difference between the two spellings is only visible for a NaN, because
    # `if (x)` in C is true for NaN and falsy in JavaScript. Kept so that the
    # claim is re-checked every run -- if a NaN ever reaches here, this line
    # turns into a failure instead of staying quietly true.
    ("JavaScript truthiness becomes C truthiness", WAV,
     "    return !(x == 0.0 || isnan(x));", "    return x != 0.0;", False),

    ("rounding -- the C library function is the wrong one", None, None, None),
    ("Math.round becomes C round()", WAV,
     """    r = floor(x);""", """    return round(x);
    r = floor(x);"""),
    ("Math.round becomes floor(x + 0.5)", WAV,
     """    y = (d >= 0.5) ? r + 1.0 : r;""",
     """    y = floor(x + 0.5);"""),
    ("ties round down instead of up", WAV,
     "    y = (d >= 0.5) ? r + 1.0 : r;",
     "    y = (d > 0.5) ? r + 1.0 : r;"),
    ("the sign of a zero result is lost", WAV,
     "    if (y == 0.0) y = signbit(x) ? -0.0 : 0.0;", "    if (0) { }"),
    ("full scale is 32768 instead of 32767", WAV,
     "        q = kl_wav_round(s * 32767.0);", "        q = kl_wav_round(s * 32768.0);"),
    ("the positive clamp is gone", WAV,
     "        if (s > 1.0) s = 1.0;", "        if (0) { }"),
    ("the negative clamp is gone", WAV,
     "        else if (s < -1.0) s = -1.0;", "        else if (0) { }"),

    ("toFixed -- printf rounds a tie the other way", None, None, None),
    ("kl_to_fixed falls through to printf", FMT,
     "    if (x >= 0.0 && x < 1e21 && (digits == 0 || digits == 2) && is_tie(x, digits)) {",
     "    if (0) {"),
    ("the tie test is off by one bit", FMT,
     "    double s = ldexp(x, digits + 1);", "    double s = ldexp(x, digits);"),
    ("a tie rounds toward zero", FMT,
     "        double n = floor(x * p10) + 1.0;", "        double n = floor(x * p10);"),

    ("the mix -- float addition is not associative", None, None, None),
    ("voices are mixed in reverse order", REN,
     "    for (vi = 0; vi < c->n_voices; vi++) {",
     "    for (vi = c->n_voices; vi-- > 0; ) {"),
    # Unreachable, and measured: every one of the corpus's 744 voice sections
    # ends with its three amplitudes at zero, so a section renders exact
    # zeros past its own totalMs -- 5,280 to 15,840 of them in the five cases
    # that have a short section. Adding zero to a float changes nothing, so
    # rendering a voice too long is not observable in the file even though it
    # is wrong. Kept so the claim is re-checked: if a section ever ends with
    # a non-zero amplitude, this line turns into a failure.
    ("each voice is rendered to the length of the utterance", REN,
     "        vn = kl_render_samples_for(v->total_ms, sample_rate);",
     "        vn = kl_render_samples_for(c->total_ms, sample_rate);", False),
    ("the mix assigns instead of adding", REN,
     "        for (i = 0; i < n; i++) out[i] += a->scratch[i];",
     "        for (i = 0; i < n; i++) out[i] = a->scratch[i];"),
    ("the sample count rounds down", REN,
     "    double n = ceil(ms * sample_rate / 1000.0);",
     "    double n = floor(ms * sample_rate / 1000.0);"),
    # The operand order claimed in kl_render.h. If this is not caught, the
    # claim is too strong and the comment says more than the measurement
    # supports -- which is worth knowing either way, so it is expected to be
    # caught and the result recorded in docs/18-stage6-wav.md.
    ("the multiply and divide are reordered", REN,
     "    double n = ceil(ms * sample_rate / 1000.0);",
     "    double n = ceil(ms / 1000.0 * sample_rate);"),
    # Unreachable, and measured: no voice section in the corpus compiles to
    # zero events, because every section ends with a trail-off. Verified over
    # all 729 cases and 744 voice sections -- zero with an empty schedule.
    ("an empty voice section is rendered anyway", REN,
     "        if (v->n_events == 0) continue;", "        if (0) continue;", False),

    ("the CLI -- the program, not the library", None, None, None),
    ("the CLI renders at 44100", CLI,
     "#define SAMPLE_RATE 48000.0", "#define SAMPLE_RATE 44100.0"),
    ("the warnings line is not printed", CLI,
     "    if (compiled.n_warnings) {", "    if (0 && compiled.n_warnings) {"),
    ("warnings are joined with a semicolon", CLI,
     '                if (printed++) fputs(", ", stderr);',
     '                if (printed++) fputs("; ", stderr);'),
    ("the byte count is printed with printf", CLI,
     "    kl_to_fixed(kb,   sizeof kb,   (double)wav_len / 1024.0, 0);",
     "    snprintf(kb, sizeof kb, \"%.0f\", (double)wav_len / 1024.0);"),
    ("the default output path changed (Windows entry)", CLI,
     '    rc = run(text, out_path ? out_path : "klattsch.wav");',
     '    rc = run(text, out_path ? out_path : "output.wav");', WIN),
    ("the default output path changed (POSIX entry)", CLI,
     '    return run(argv[1], (argc >= 3) ? argv[2] : "klattsch.wav");',
     '    return run(argv[1], (argc >= 3) ? argv[2] : "output.wav");', not WIN),
    # Windows-only by construction: both live inside #ifdef _WIN32, so on a
    # Unix-like the mutation compiles and changes nothing. The expectation
    # follows the platform rather than being waived on one of them.
    ("arguments are converted through the ANSI codepage", CLI,
     """    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) return NULL;
    s = malloc((size_t)n);
    if (!s) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {""",
     """    int n = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) return NULL;
    s = malloc((size_t)n);
    if (!s) return NULL;
    if (WideCharToMultiByte(CP_ACP, 0, w, -1, s, n, NULL, NULL) <= 0) {""", WIN),
    ("the output file is opened with fopen", CLI,
     '        f = _wfopen(w, L"wb");', '        f = fopen(path_u8, "wb");', WIN),
]

MUTATIONS = [(m + (True,)) if len(m) == 4 else m for m in MUTATIONS]
# A mutation may name several sites, because some mistakes are only a mistake
# when they are made consistently -- see the empty software field above.
MUTATIONS = [(lbl, tgt,
              f if (f is None or isinstance(f, list)) else [f],
              r if (r is None or isinstance(r, list)) else [r], e)
             for lbl, tgt, f, r, e in MUTATIONS]

FILES = sorted({m[1] for m in MUTATIONS if m[1] is not None})


def write_lf(path, text):
    """Write without newline translation.

    .gitattributes pins *.c to LF so that four compilers on two operating
    systems read identical bytes. Path.write_text() on Windows translates to
    os.linesep, so the naive version of this rewrites the file as CRLF --
    including on the restore path, which undoes the pinning the moment the
    suite is run once. Found in stage 4 and written down there.
    """
    path.write_bytes(text.encode("utf-8"))


def run(cmd, timeout=BUILD_TIMEOUT):
    try:
        # errors='replace': the verifier prints case text and some of it is
        # not ASCII. Python's default here is the console code page, and
        # cp1252 choking on a corpus case killed the reader thread mid-run in
        # stage 4 and was reported as a harness timeout on the next mutation.
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                           encoding='utf-8', errors='replace', timeout=timeout)
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
    argv = sys.argv[1:]
    rate = None
    if "--rate" in argv:
        i = argv.index("--rate")
        rate = argv[i + 1]
        del argv[i:i + 2]
    build = Path(argv[0] if argv else "build")
    if not (ROOT / build).is_dir():
        print("usage: python tools/stage6-mutations.py <configured-build-dir>", file=sys.stderr)
        return 2

    rel = [str(p.relative_to(ROOT)) for p in FILES]
    rc, _ = run(["git", "diff", "--quiet", "--"] + rel, timeout=60)
    if rc != 0:
        print("refusing to run: one of " + ", ".join(rel)
              + " has uncommitted changes, and this reverts with git", file=sys.stderr)
        return 2

    dump = find_tool(ROOT / build, "kl_wav_dump")
    cli = find_tool(ROOT / build, "klattsch")
    if dump is None:
        print(f"no kl_wav_dump in {build}", file=sys.stderr)
        return 2

    verify = ["node", "tools/verify-stage6.mjs", str(dump)]
    # One rate instead of three is about four times faster and catches every
    # mutation in this file. The exit test itself -- all three rates -- is
    # what ctest runs; this is the meta-test, and CI uses it to keep the job
    # inside its timeout.
    if rate:
        verify += ["--rate", rate]
    if cli is not None:
        verify += ["--cli", str(cli)]
    else:
        print("note: no klattsch executable found; the CLI mutations cannot be caught",
              file=sys.stderr)

    original = {p: p.read_text(encoding="utf-8") for p in FILES}
    passed = failed = retries = 0

    print("Breaking the encoder, the mix and the CLI; each line must be caught.\n")
    try:
        for label, target, find, repl, expect_caught in MUTATIONS:
            if target is None:
                print(label, flush=True)
                continue

            text = original[target]
            bad = next((f for f in find if text.count(f) != 1), None)
            if bad is not None:
                print(f"  {label:<52} SKIP (pattern matches {text.count(bad)} times)", flush=True)
                failed += 1
                continue
            for f, r in zip(find, repl):
                text = text.replace(f, r, 1)
            write_lf(target, text)

            rc, timed_out = run(["cmake", "--build", str(build), "--config", "Release"])
            if timed_out:
                verdict = "HARNESS TIMEOUT (build)"
                failed += 1
            elif rc != 0:
                verdict = "caught (did not compile)"
                passed += 1
            else:
                rc, timed_out = run(verify, timeout=VERIFY_TIMEOUT)
                if timed_out:
                    # Measured: this verifier runs in about half a minute, so
                    # a timeout is a hang rather than slowness. Retry once and
                    # say so in the summary -- smoothing a flake over in
                    # silence is how a suite stops meaning anything, and
                    # reporting a hang as a mutation result is a false
                    # negative. Stage 4 hit this twice.
                    retries += 1
                    rc, timed_out = run(verify, timeout=VERIFY_TIMEOUT)
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

            write_lf(target, original[target])
            run(["cmake", "--build", str(build), "--config", "Release"])
    finally:
        for p in FILES:
            write_lf(p, original[p])
        run(["cmake", "--build", str(build), "--config", "Release"])

    print(f"\ncaught {passed}, missed {failed}"
          + (f" -- {retries} verify retry(s) after a harness hang" if retries else ""))
    if failed:
        print("A mutation slipped through: the comparison does not reach that path.",
              file=sys.stderr)
        return 1
    print("All mutations caught.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
