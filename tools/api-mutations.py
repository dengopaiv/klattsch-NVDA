#!/usr/bin/env python3
"""Exit test for step 5: prove the API's tests catch a broken API.

    python tools/api-mutations.py <build-dir>

kl_api_test passed on its first run, on every toolchain. That is what this
suite is for: a test that has never failed is either a correct API or a test
that is not looking, and only breaking the API on purpose tells them apart.

Five kinds of thing to break:

  * the *mix* -- kl_stream must be kl_render_mix exactly, so voice order,
    voice length, the per-voice schedule slice and the clock all matter;
  * the *index places* -- which phrase an index belongs to, which end of it,
    how milliseconds become a sample, and the offset arithmetic for a piece
    that is not ASCII;
  * the *joins and silences* -- the separator between pieces, a break of 0,
    what counts as sounding, and when an utterance renders no audio at all;
  * *cancel* -- every index still comes out once;
  * the *level and the front-end option* -- volume, and inflection, whose
    guard is what keeps the goldens unchanged.

Each mutation is run against `kl_api_test stream`, `kl_api_test api`, and
tools/verify-text.mjs (the front end against its goldens), and must make at
least one of them fail. Four are expected not to, and say why: they are the
places where the code is stricter than anything observable, and are listed
so that the next reader knows that rather than finding out. Two of the four
were expected to be caught when this suite was written; its first run showed
otherwise, and docs/21-stage-api.md §21.5 records both.

Every pattern must occur exactly once in its file.

Exit 0 means every mutation that should be caught was.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
API = ROOT / "csrc" / "kl_api.c"
STREAM = ROOT / "csrc" / "kl_stream.c"
TEXT = ROOT / "csrc" / "kl_text.c"

BUILD_TIMEOUT = 300
VERIFY_TIMEOUT = 600

# (label, file, find, replace[, expect_caught])
MUTATIONS = [
    ("the mix", None, None, None),
    ("voices summed in reverse order", STREAM,
     "    for (vi = 0; vi < s->c->n_voices; vi++) {\n        size_t vn = s->voice_n[vi], m;",
     "    for (vi = s->c->n_voices; vi-- > 0;) {\n        size_t vn = s->voice_n[vi], m;"),
    ("every voice shares the first schedule slice", STREAM,
     "        off += v->n_events;",
     "        off += 0;"),
    ("a voice's clock starts one sample late", STREAM,
     "                       s->at_sample + off, s->transition_len + off, 0);",
     "                       s->at_sample + off, s->transition_len + off, -1);"),
    ("a voice is rendered on past its own end", STREAM,
     "        m = vn - s->pos < n ? vn - s->pos : n;",
     "        m = n;",
     False),
    ("the mix is not cleared before a chunk", STREAM,
     "    memset(out, 0, n * sizeof *out);",
     "    (void)0;"),

    ("index places", None, None, None),
    ("an index takes the phrase ending after it", API,
     "                   (size_t)v->phrases[k].src_end <= h->idx[i].units) {",
     "                   (size_t)v->phrases[k].src_start <= h->idx[i].units) {"),
    ("an index fires at its phrase's start", API,
     "                t = v->phrases[k].t_end_ms;",
     "                t = v->phrases[k].t_start_ms;"),
    ("milliseconds to samples by floor", API,
     "        s = kl_render_samples_for(t, h->sr);",
     "        s = (size_t)(t * h->sr / 1000.0);"),
    ("a non-ASCII piece counted in bytes", API,
     "    if (i == n) return (long)n;",
     "    if (1) return (long)n;"),
    ("an index is not capped at the end", API,
     "        h->idx[i].sample = s < h->S.total ? s : h->S.total;",
     "        h->idx[i].sample = s;"),

    ("joins and silences", None, None, None),
    ("pieces joined by a space", API,
     "    if (sep) h->src[h->src_len++] = '\\n';",
     "    if (sep) h->src[h->src_len++] = ' ';"),
    # Expected to survive: the silence event `p0` emits shares its sample with
    # the next event, which overrides every amplitude. Kept as the record of
    # a claim that was wrong (kl_api_break's comment).
    ("a break of 0 is written as p0", API,
     "    if (ms <= 0) return KL_API_OK;",
     "    if (ms < 0) return KL_API_OK;",
     False),
    ("unknown phonemes counted as sounding", API,
     "                n--;\n",
     "                (void)0;\n"),
    ("an utterance with nothing to hear renders anyway", API,
     "    if (h->sounding <= 0 && !has_pause)\n        return no_audio(h, 0);",
     "    if (0)\n        return no_audio(h, 0);"),
    ("the silence phoneme counted as sounding", API,
     "                if (p->phoneme_len == 1 && p->phoneme[0] == '_') *has_pause = 1;\n                else n++;",
     "                n++;",
     False),

    ("cancel", None, None, None),
    ("a cancel drops the indexes still to come", API,
     "        (h->cancelled || h->idx[h->idx_next].sample <= h->S.pos)) {",
     "        (h->idx[h->idx_next].sample <= h->S.pos)) {"),
    ("a cancel is ignored", API,
     "    if (h) h->cancelled = 1;",
     "    (void)h;"),

    ("level and inflection", None, None, None),
    ("volume ignored", API,
     "    g = KL_API_OUTPUT_GAIN * h->set[KL_SET_VOLUME];",
     "    g = KL_API_OUTPUT_GAIN;"),
    ("rounded by truncation", API,
     "        q = kl_wav_round(s * 32767.0);",
     "        q = (double)(long)(s * 32767.0);"),
    # Expected to survive: floor(x + 0.5) and Math.round part only at
    # 0.49999999999999994 and in the sign of zero, and neither reaches an
    # int16. kl_wav_round is used to agree with kl_wav, not for the sound.
    ("rounded by floor(x + 0.5)", API,
     "        q = kl_wav_round(s * 32767.0);",
     "        q = floor(s * 32767.0 + 0.5);",
     False),
    ("inflection applied without its guard", TEXT,
     "    if (opts && opts->set_inflection)\n        base *= ",
     "    if (opts)\n        base *= "),
    ("inflection never applied", TEXT,
     "        base *= opts->inflection > 0.0 ? opts->inflection : 0.0;",
     "        base *= 1.0;"),
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
        print("usage: python tools/api-mutations.py <configured-build-dir>", file=sys.stderr)
        return 2

    rel = [str(p.relative_to(ROOT)) for p in FILES]
    rc, _ = run(["git", "diff", "--quiet", "--"] + rel, timeout=60)
    if rc != 0:
        print("refusing to run: one of " + ", ".join(rel)
              + " has uncommitted changes, and this reverts with git", file=sys.stderr)
        return 2

    test = find_tool(ROOT / build, "kl_api_test")
    dump = find_tool(ROOT / build, "kl_text_dump")
    if test is None or dump is None:
        print(f"no kl_api_test or kl_text_dump in {build}", file=sys.stderr)
        return 2
    checks = [
        [str(test), "stream", str(ROOT / "goldens" / "cases-compile.bin")],
        [str(test), "api", str(ROOT / "tools" / "text-corpus.txt")],
        ["node", "tools/verify-text.mjs", str(dump)],
    ]

    original = {p: p.read_text(encoding="utf-8") for p in FILES}
    passed = failed = 0

    print("Breaking the mix, the indexes, the joins, cancel and the level; "
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
                verdict, ok = "caught (did not compile)", expect_caught
            else:
                caught_by = None
                for chk in checks:
                    rc, timed_out = run(chk, timeout=VERIFY_TIMEOUT)
                    if timed_out:
                        caught_by = "TIMEOUT"
                        break
                    if rc != 0:
                        caught_by = Path(chk[0]).stem if chk[0] != "node" else "verify-text"
                        if chk[0] != "node":
                            caught_by += " " + chk[1]
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
