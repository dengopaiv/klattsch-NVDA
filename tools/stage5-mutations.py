#!/usr/bin/env python3
"""Exit test for stage 5: prove the schedule comparison catches a broken compiler.

    python tools/stage5-mutations.py <build-dir>

Stage 5 passed its verifier on the first run, which is the least reassuring
way for the highest-risk stage in the plan to pass. This suite is what decides
whether that meant the translation was right or the comparison was blind.

There are six kinds of thing to break here, and a suite that only broke one of
them would be reassuring for the wrong reason:

  * the *layering* in emit() -- phoneme, then extras, then voice state. Each
    overwrites the last, and a mechanical translation gets the order backwards
    without producing anything that looks wrong.
  * the *shape* of each branch of renderPhoneme -- the fractions and the caps.
    These are pure arithmetic on a slot width and every one of them is a
    plausible typo.
  * the *time accumulation* -- when timeMs advances relative to when an event
    is emitted. Swapping the two lines moves every following event.
  * the *section* boundaries -- voices compile from a fresh initial state, and
    totalMs is the maximum rather than voice 0's.
  * the *initial* values a bare-letter reset returns to, which come from opts
    and not from the defaults.
  * the *strings* -- warnings are as much the contract as the numbers are.

Unlike the stage 3 and 4 suites, this one requires each pattern to occur
exactly once. Those suites replaced the first match, which is silently the
wrong edit when a later change makes a pattern ambiguous.

Exit 0 means every mutation was caught.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "csrc" / "kl_compile.c"

BUILD_TIMEOUT = 180
VERIFY_TIMEOUT = 300

# (label, file, find, replace[, expect_caught])
MUTATIONS = [
    ("the three layers of emit() -- each overwrites the last", None, None, None),
    # extras must beat the phoneme's own fields, and voice state must beat
    # both. Applying them in the other order is the mechanical translation.
    # Unreachable, and measured rather than assumed: an extras key is only
    # created by the /^[A-Z]/ branch, so it always begins with an uppercase
    # ASCII letter, and all seven voice-state parameters -- vibratoDepth,
    # vibratoRate, tremoloDepth, tremoloRate, aspiration, tilt, effort --
    # begin with a lowercase one. The two key spaces cannot intersect, so the
    # order of those two layers is not observable. Kept, with the claim
    # re-checked on every run: if a lowercase extras path is ever added, this
    # line turns into a failure instead of staying quietly true.
    ("voice state applied before the extras", SRC,
     """    for (i = 0; i < S->n_ex; i++) {
        int pi = param_index(S->ex[i].key, S->ex[i].key_len);
        if (pi >= 0) tset(t, pi, S->ex[i].value);
    }

    tset(t, KL_VIBRATO_DEPTH, S->vibrato);""",
     """    tset(t, KL_VIBRATO_DEPTH, S->vibrato);
    for (i = 0; i < S->n_ex; i++) {
        int pi = param_index(S->ex[i].key, S->ex[i].key_len);
        if (pi >= 0) tset(t, pi, S->ex[i].value);
    }
""", False),
    ("an extra naming a parameter no longer overrides it", SRC,
     "        if (pi >= 0) tset(t, pi, S->ex[i].value);",
     "        if (0 && pi >= 0) tset(t, pi, S->ex[i].value);"),
    ("effort is no longer carried into the target", SRC,
     "    tset(t, KL_EFFORT,        S->effort);",
     "    (void)0;"),
    ("seeded opts.extras are dropped", SRC,
     "            S.ex[S.n_ex].value   = opts->extras[i].value;",
     "            S.ex[S.n_ex].value   = opts->extras[i].value;\n            continue;"),
    ("a bare uppercase directive no longer clears the extra", SRC,
     "        if (t->reset) extras_delete(S, k, klen);",
     "        if (0 && t->reset) extras_delete(S, k, klen);"),
    ("the extras of a target come out unsorted", SRC,
     "    if (c) return c < 0;", "    if (c) return c > 0;"),

    ("scaled() -- what the running formant scale reaches", None, None, None),
    ("bandwidths are no longer scaled", SRC,
     "    tset(t, KL_BW1, p->BW1 * S->scale);",
     "    tset(t, KL_BW1, p->BW1);"),
    ("amplitudes are scaled too", SRC,
     "    tset(t, KL_A1, p->A1);", "    tset(t, KL_A1, p->A1 * S->scale);"),
    ("the glide endpoint is ignored for F2", SRC,
     "    tset(t, KL_FF2, (g ? g->F2 : p->F2) * S->scale);",
     "    tset(t, KL_FF2, p->F2 * S->scale);"),

    ("renderPhoneme -- the four shapes", None, None, None),
    ("stops render as steady vowels", SRC,
     "    if (p->is_stop) {", "    if (0 && p->is_stop) {"),
    ("diphthongs render as steady vowels", SRC,
     "    } else if (p->has_glide) {", "    } else if (0 && p->has_glide) {"),
    ("a negative pitch delta takes the steady path", SRC,
     "    } else if (t->pitch_delta != 0.0) {",
     "    } else if (t->pitch_delta > 0.0) {"),
    ("stop burst fraction 0.3 -> 0.35", SRC,
     "js_min(KL_DEFAULT_STOP_BURST_MS, slot_ms * 0.3);",
     "js_min(KL_DEFAULT_STOP_BURST_MS, slot_ms * 0.35);"),
    ("stop pre-silence transition cap 20 -> 18", SRC,
     "        emit_silence(S, js_min(20.0, silence_ms * 0.4));",
     "        emit_silence(S, js_min(18.0, silence_ms * 0.4));"),
    # Unreachable, and this one is a dead literal in the *reference*:
    # burstMs = Math.min(25, slotMs * 0.3) is at most 25, so burstMs * 0.2 is
    # at most 5, so the 5 never binds. Verified over 402,001 slot widths from
    # -500 to 100,000 ms: the cap changed the answer zero times. Raising it
    # therefore cannot change a schedule, whatever the corpus contains.
    ("stop burst transition cap 5 -> 6", SRC,
     "        emit(S, &g, js_min(5.0, burst_ms * 0.2));",
     "        emit(S, &g, js_min(6.0, burst_ms * 0.2));", False),
    ("glide onset 0.25 -> 0.2", SRC,
     "        double onset  = slot_ms * 0.25;",
     "        double onset  = slot_ms * 0.2;"),
    ("glide offset never accumulates", SRC,
     "        S->time_ms += glide + offset;", "        S->time_ms += glide;"),
    ("the glide endpoint keeps the starting pitch", SRC,
     "        scaled(S, p, end_f0, &p->glide_to, &g);",
     "        scaled(S, p, start_f0, &p->glide_to, &g);"),
    ("pitch-move second leg 0.6 -> 0.5", SRC,
     "        emit(S, &g, slot_ms * 0.6);", "        emit(S, &g, slot_ms * 0.5);"),
    ("pitch-move tail 0.75 -> 0.7", SRC,
     "        S->time_ms += slot_ms * 0.75;", "        S->time_ms += slot_ms * 0.7;"),
    ("steady transition cap 35 -> 40", SRC,
     "        double trans = js_min(KL_DEFAULT_TRANSITION_MS, slot_ms * 0.4);",
     "        double trans = js_min(40.0, slot_ms * 0.4);"),
    ("steady transition fraction 0.4 -> 0.45", SRC,
     "js_min(KL_DEFAULT_TRANSITION_MS, slot_ms * 0.4);",
     "js_min(KL_DEFAULT_TRANSITION_MS, slot_ms * 0.45);"),

    ("time accumulates in an order a translation can invert", None, None, None),
    ("the stop's silence is emitted after time advances", SRC,
     """        emit_silence(S, js_min(20.0, silence_ms * 0.4));
        S->time_ms += silence_ms;""",
     """        S->time_ms += silence_ms;
        emit_silence(S, js_min(20.0, silence_ms * 0.4));"""),
    ("an unknown phoneme consumes its slot anyway", SRC,
     """        warn_parts(S, "unknown phoneme: ", S->A + t->code_off, t->code_len);
        return;""",
     """        warn_parts(S, "unknown phoneme: ", S->A + t->code_off, t->code_len);
        S->time_ms += slot_ms;
        return;"""),
    ("the fade-out transition 100 -> 90", SRC,
     "    emit_silence(&S, KL_DEFAULT_FADE_OUT_MS);",
     "    emit_silence(&S, 90.0);"),
    ("the trail-off 150 -> 140", SRC,
     "    S.time_ms += KL_DEFAULT_TRAIL_OFF_MS;", "    S.time_ms += 140.0;"),
    ("the final phrase is no longer held to the end", SRC,
     "    if (V->n_phrases) V->phrases[V->n_phrases - 1].t_end_ms = S.time_ms;",
     "    if (0) V->phrases[V->n_phrases - 1].t_end_ms = S.time_ms;"),

    ("stress, syllables and pauses", None, None, None),
    ("the stress pitch lift 8 -> 9", SRC,
     "    start_f0 = t->stressed ? S->f0 + KL_DEFAULT_STRESS_F0_LIFT : S->f0;",
     "    start_f0 = t->stressed ? S->f0 + 9.0 : S->f0;"),
    ("stress no longer lengthens the phoneme", SRC,
     """                double phone_rate = t->stressed
                    ? S.rate * KL_DEFAULT_STRESS_DURATION_FACTOR : S.rate;""",
     "                double phone_rate = S.rate;"),
    ("a group does not divide its slot", SRC,
     "    slot = S->rate / (double)S->n_syl;", "    slot = S->rate;"),
    ("a sticky delta inside a group does not accumulate", SRC,
     """        render_phoneme(S, t, slot);
        emit_phrase(S, t);
        if (!t->transient) S->f0 += t->pitch_delta;""",
     """        render_phoneme(S, t, slot);
        emit_phrase(S, t);"""),
    ("an unclosed group is discarded instead of flushed", SRC,
     """        warn_parts(&S, "unclosed (", NULL, 0);
        flush_syllable(&S);""",
     """        warn_parts(&S, "unclosed (", NULL, 0);"""),
    ("a transient delta accumulates like a sticky one", SRC,
     """                render_phoneme(&S, t, phone_rate);
                emit_phrase(&S, t);
                if (!t->transient) S.f0 += t->pitch_delta;""",
     """                render_phoneme(&S, t, phone_rate);
                emit_phrase(&S, t);
                S.f0 += t->pitch_delta;"""),
    ("the pause directive stops taking the absolute value", SRC,
     "        S->time_ms += fabs(t->value);", "        S->time_ms += t->value;"),
    ("a pause token uses the default silence length", SRC,
     "            S.time_ms += t->ms;", "            S.time_ms += 30.0;"),

    ("directives -- reset, relative, and where initial values come from", None, None, None),
    # Unreachable: no token is ever both. classifyPart produces `reset` only
    # from the bare forms, which carry no value and no sign, and `relative`
    # only from the signed compact form, which carries both. Measured over the
    # corpus: 455 directive tokens, 0 with both flags set.
    ("reset and relative swap precedence", SRC,
     """    if (t->reset)         *cur = initial;
    else if (t->relative) *cur += t->value;""",
     """    if (t->relative)      *cur += t->value;
    else if (t->reset)    *cur = initial;""", False),
    ("a relative directive assigns instead of adding", SRC,
     "    else if (t->relative) *cur += t->value;",
     "    else if (t->relative) *cur = t->value;"),
    ("[pitch=N] is no longer the same state as b", SRC,
     'if (span_eq_lit(k, klen, "base") || span_eq_lit(k, klen, "pitch")) {',
     'if (span_eq_lit(k, klen, "base")) {'),
    ("a bare reset returns to the default, not to opts", SRC,
     "    S.i_rate         = opt_or(opts, KL_OPT_RATE,          KL_DEFAULT_RATE);",
     "    S.i_rate         = KL_DEFAULT_RATE;"),
    ("[bank] resets to the default bank, not to opts", SRC,
     "        case KL_TOK_BANK_RESET:\n            S.bank = S.i_bank;",
     "        case KL_TOK_BANK_RESET:\n            S.bank = kl_bank_default();"),
    ("[engine] resets to nothing, not to opts", SRC,
     "        case KL_TOK_ENGINE_RESET:\n            S.engine     = S.i_engine;",
     "        case KL_TOK_ENGINE_RESET:\n            S.engine     = NULL;"),
    ("an unknown bank switches to the default instead of warning", SRC,
     'if (!b) warn_parts(&S, "unknown bank: ", S.A + t->name_off, t->name_len);',
     'if (!b) S.bank = kl_bank_default();'),

    ("voice sections", None, None, None),
    ("totalMs is voice 0's, not the maximum", SRC,
     "        if (s == 0 || out->voices[s].total_ms > out->total_ms)",
     "        if (s == 0)"),
    ("a section's phrases start at the input, not at the marker", SRC,
     "                if (++seen == s) { start = T->tokens[i].src_end; break; }",
     "                if (++seen == s) { start = 0; break; }"),
    ("only voice 0's warnings are reported", SRC,
     "    out->n_warnings = wn;", "    out->n_warnings = out->voices[0].n_warnings;"),
    ("[voice=N] is no longer a section marker", SRC,
     """    return t->type == KL_TOK_DIRECTIVE
        && span_eq_lit(T->arena + t->key_off, t->key_len, "voice");""",
     "    (void)T; (void)t;\n    return 0;"),

    ("phrase spans", None, None, None),
    ("a phrase starts at its own token, not where the last one ended", SRC,
     "    ph->src_start       = S->phrase_src_start;",
     "    ph->src_start       = t->src_start;"),
    ("a phrase's time starts at its own event", SRC,
     "    ph->t_start_ms      = S->phrase_time_start;",
     "    ph->t_start_ms      = S->time_ms;"),

    ("warning strings -- as much the contract as the numbers", None, None, None),
    ("the unknown-token warning loses its text", SRC,
     'warn_parts(&S, "unknown token: ", S.A + t->text_off, t->text_len);',
     'warn_parts(&S, "unknown token: ", NULL, 0);'),
    ("the nested-group warning is reworded", SRC,
     '"nested ( ignored"', '"nested ( skipped"'),
    ("an unmatched ) is silently ignored", SRC,
     'if (!S.in_syllable) { warn_parts(&S, "unmatched )", NULL, 0); break; }',
     'if (!S.in_syllable) { break; }'),
]

MUTATIONS = [(m + (True,)) if len(m) == 4 else m for m in MUTATIONS]


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
        # errors='replace': the verifier prints token text and some of it is
        # not ASCII. Python's default here is the console code page, and
        # cp1252 choking on U+00A8 killed the reader thread mid-run in stage 4
        # and was reported as a harness timeout on the *next* mutation.
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                           encoding='utf-8', errors='replace', timeout=timeout)
        return r.returncode, False
    except subprocess.TimeoutExpired:
        return None, True


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build")
    if not (ROOT / build).is_dir():
        print("usage: python tools/stage5-mutations.py <configured-build-dir>", file=sys.stderr)
        return 2

    rc, _ = run(["git", "diff", "--quiet", "--", str(SRC.relative_to(ROOT))], timeout=60)
    if rc != 0:
        print(f"refusing to run: {SRC.name} has uncommitted changes, and this reverts with git",
              file=sys.stderr)
        return 2

    dump = ROOT / build / "kl_compile_dump.exe"
    if not dump.exists():
        dump = ROOT / build / "Release" / "kl_compile_dump.exe"
    if not dump.exists():
        dump = ROOT / build / "kl_compile_dump"
    if not dump.exists():
        print(f"no kl_compile_dump in {build}", file=sys.stderr)
        return 2

    original = SRC.read_text(encoding="utf-8")
    passed = failed = retries = 0

    print("Breaking the compiler; each line must be caught.\n")
    try:
        for entry in MUTATIONS:
            label, target, find, repl = entry[0], entry[1], entry[2], entry[3]
            expect_caught = entry[4] if len(entry) > 4 else True
            if target is None:
                print(label, flush=True)
                continue

            n = original.count(find)
            if n != 1:
                # Not "pattern not found": a pattern that matches twice edits
                # a place this suite did not name, and reports whatever that
                # does as the result for this line.
                print(f"  {label:<52} SKIP (pattern matches {n} times)", flush=True)
                failed += 1
                continue
            write_lf(target, original.replace(find, repl, 1))

            rc, timed_out = run(["cmake", "--build", str(build), "--config", "Release"])
            if timed_out:
                verdict = "HARNESS TIMEOUT (build)"
                failed += 1
            elif rc != 0:
                verdict = "caught (did not compile)"
                passed += 1
            else:
                rc, timed_out = run(["node", "tools/verify-stage5.mjs", str(dump)],
                                    timeout=VERIFY_TIMEOUT)
                if timed_out:
                    # Measured: this verifier runs in a couple of seconds, so
                    # a timeout is a hang rather than slowness. Retry once and
                    # say so in the summary -- smoothing a flake over in
                    # silence is how a suite stops meaning anything, and
                    # reporting a hang as a mutation result is a false
                    # negative. Stage 4 hit this twice.
                    retries += 1
                    rc, timed_out = run(["node", "tools/verify-stage5.mjs", str(dump)],
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

            write_lf(target, original)
            run(["cmake", "--build", str(build), "--config", "Release"])
    finally:
        write_lf(SRC, original)
        run(["cmake", "--build", str(build), "--config", "Release"])

    print(f"\ncaught {passed}, missed {failed}"
          + (f" -- {retries} verify retry(s) after a harness hang" if retries else ""))
    if failed:
        print("A mutation slipped through: the corpus does not reach that path.", file=sys.stderr)
        return 1
    print("All mutations caught.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
