#!/usr/bin/env python3
"""Exit test for stage 3: prove the sample-loop comparison catches a broken synth.

    python tools/stage3-mutations.py <build-dir>

Unlike stages 0 and 2, the interesting mutations here are not constants but
*logic*: the order of the drain and the counter, which parameters get a fresh
increment, whether the transition snaps at the end, what tilt_prev holds. Those
are what a mechanical translation gets wrong, and a constant-only suite would
not notice any of them.

Written in Python rather than shell because the patterns contain `/` and span
several lines, which breaks `perl -pi -e s///` in two different ways -- the
first version of this script reported eight false "NOT CAUGHT" results that
were its own quoting failures. A mutation runner that cannot express the
mutation is worse than none, because it reports a passing test as a gap.

Exit 0 means every mutation was caught.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "csrc" / "kl_synth.c"
PREP = ROOT / "csrc" / "kl_synth.c"

# (label, find, replace) -- all literal, no regex.
MUTATIONS = [
    ("constants", None, None),
    ("default gain 3.5 -> 3.51",
     "3.5,        /* gain", "3.51,       /* gain"),
    ("default effort 0.5 -> 0.51",
     "0.5         /* effort", "0.51        /* effort"),
    ("voiced gain 0.85 -> 0.851",
     "cur[KL_ASPIRATION] * 0.85;", "cur[KL_ASPIRATION] * 0.851;"),
    ("unvoiced noise 0.35 -> 0.351",
     "noise * 0.35", "noise * 0.351"),
    ("aspiration noise 0.5 -> 0.501",
     "noise * 0.5;", "noise * 0.501;"),
    ("LFSR seed",
     "(int32_t)0xACE1ACE1u", "(int32_t)0xACE1ACE2u"),

    ("logic -- what a mechanical translation gets wrong", None, None),
    # Only the parameters an event carries get a fresh increment. The JS
    # recomputes every one, which is the difference between a parameter
    # resuming its glide from where it actually is and jumping.
    ("increment only for present params",
     "        if (present & (1u << k)) s->target[k] = values[k];\n"
     "        s->increment[k] = (s->target[k] - s->current[k]) / dn;",
     "        if (present & (1u << k)) { s->target[k] = values[k];\n"
     "        s->increment[k] = (s->target[k] - s->current[k]) / dn; }"),
    # The snap on the last sample of a transition, which stops drift.
    ("no snap at end of transition",
     "if (s->transition_samples == 0) {", "if (0) {"),
    # The drain happens at the current counter, then the counter advances.
    ("counter advanced before the drain",
     "        while (s->schedule_idx < s->schedule_len\n"
     "               && s->at_sample[s->schedule_idx] <= s->sample_counter) {",
     "        s->sample_counter++;\n"
     "        while (s->schedule_idx < s->schedule_len\n"
     "               && s->at_sample[s->schedule_idx] <= s->sample_counter) {"),
    # tilt_prev holds the previous *untilted* y; holding the tilted value
    # turns a one-zero filter into a one-pole one.
    ("tilt feeds back on itself",
     "s->tilt_prev = y;", "s->tilt_prev = tilted;"),
    # The noise sample is the SIGNED state over 2^31.
    ("noise unsigned instead of signed",
     "noise = (double)s->lfsr / 2147483648.0;",
     "noise = (double)(uint32_t)s->lfsr / 2147483648.0;"),
    ("tremolo bipolar",
     "(0.5 + 0.5 * sin(s->tremolo_phase))", "(0.0 + 1.0 * sin(s->tremolo_phase))"),
    ("glottal phase truncates not floors",
     "s->glottal_phase -= floor(s->glottal_phase);",
     "s->glottal_phase -= (double)(long)s->glottal_phase;"),
    ("transition length max(1) dropped",
     "transition_len[i] = n < 1.0 ? 1L : (long)n;",
     "transition_len[i] = (long)n;"),
    ("unreachable through the compiler -- expected NOT to be caught", None, None),
    # atMs is never negative: the compiler's timeMs starts at 0 and only
    # increases, and all 599 events in the corpus have atMs >= 0. So floor and
    # truncation agree on every input the compiler can produce, and no corpus
    # extension can change that -- it would take a hand-built schedule.
    # Kept, and expected to pass, so the claim is re-checked on every run
    # rather than resting on a measurement made once.
    ("atSample truncates not floors (atMs >= 0 always)",
     "at_sample[i] = (long)floor(events[i].at_ms * sample_rate / 1000.0);",
     "at_sample[i] = (long)(events[i].at_ms * sample_rate / 1000.0);",
     False),
]

# Entries are (label, find, replace) or (label, find, replace, expect_caught).
MUTATIONS = [(m + (True,)) if len(m) == 3 and m[1] is not None else m for m in MUTATIONS]


# Every subprocess gets a deadline. The first run of this suite stalled for
# forty minutes on a step that takes five seconds, with a mutation still
# applied to the working tree; the code under test was fine, the harness was
# not. A mutation runner that can hang is a mutation runner that will, and it
# leaves the source broken while it does.
BUILD_TIMEOUT = 180
VERIFY_TIMEOUT = 300


def write_lf(path, text):
    """Write without newline translation.

    .gitattributes pins *.c to LF so that four compilers on two operating
    systems read identical bytes. Path.write_text() on Windows translates
    to os.linesep, so the naive version of this script rewrote the file as
    CRLF -- including when restoring the original after a mutation, which
    left the working tree permanently converted and undid the pinning the
    moment the suite was run once.
    """
    path.write_bytes(text.encode("utf-8"))


def run(cmd, timeout=BUILD_TIMEOUT, **kw):
    """Run a command with a deadline. Returns (returncode, timed_out)."""
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                           timeout=timeout, **kw)
        return r.returncode, False
    except subprocess.TimeoutExpired:
        return None, True


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build-msvc")
    if not (ROOT / build).is_dir():
        print(f"usage: python tools/stage3-mutations.py <configured-build-dir>", file=sys.stderr)
        return 2

    rc, _ = run(["git", "diff", "--quiet", "--", str(SRC.relative_to(ROOT))], timeout=60)
    if rc != 0:
        print(f"refusing to run: {SRC.name} has uncommitted changes, and this reverts with git",
              file=sys.stderr)
        return 2

    dump = ROOT / build / "kl_synth_dump.exe"
    if not dump.exists():
        dump = ROOT / build / "kl_synth_dump"
    if not dump.exists():
        print(f"no kl_synth_dump in {build}", file=sys.stderr)
        return 2

    original = SRC.read_text(encoding="utf-8")
    passed = failed = retries = 0

    print("Breaking the sample loop; each line must be caught.\n")
    try:
        for entry in MUTATIONS:
            label, find, repl = entry[0], entry[1], entry[2]
            expect_caught = entry[3] if len(entry) > 3 else True
            if find is None:
                print(label, flush=True)
                continue

            text = original
            if find not in text:
                print(f"  {label:<48} SKIP (pattern not found)", flush=True)
                failed += 1
                continue
            write_lf(SRC, text.replace(find, repl, 1))

            rc, timed_out = run(["cmake", "--build", str(build)])
            if timed_out:
                verdict = "HARNESS TIMEOUT (build)"
                failed += 1
            elif rc != 0:
                verdict = "caught (did not compile)"
                passed += 1
            else:
                # All three sample rates, not just 48 kHz. The `max(1, ...)`
                # floor on the transition length only binds at 8 kHz -- at
                # 48 kHz the shortest transition the compiler can emit is
                # still 2.9 samples -- so a 48-kHz-only suite reported that
                # mutation as a corpus gap when the corpus was fine and the
                # suite was looking in the wrong place.
                rc, timed_out = run(["node", "tools/verify-stage3.mjs", str(dump)],
                                    timeout=VERIFY_TIMEOUT)
                if timed_out:
                    # Measured: this verifier runs in well under a second, so a timeout is
                    # a hang rather than slowness. It has been seen when several suites run
                    # against the same build directory at once. Retry once, and say so in
                    # the summary -- smoothing a flake over in silence is how a suite stops
                    # meaning anything, and reporting a hang as a mutation result is a
                    # false negative.
                    retries += 1
                    rc, timed_out = run(["node", "tools/verify-stage3.mjs", str(dump)],
                                        timeout=VERIFY_TIMEOUT)
                if timed_out:
                    verdict = "HARNESS TIMEOUT (verify)"
                    failed += 1
                elif rc == 0:
                    if expect_caught:
                        verdict = "NOT CAUGHT  <-- gap in the corpus"
                        failed += 1
                    else:
                        verdict = "not caught, as expected"
                        passed += 1
                else:
                    if expect_caught:
                        verdict = "caught"
                        passed += 1
                    else:
                        verdict = "CAUGHT -- the unreachable path is reachable now"
                        failed += 1
            print(f"  {label:<48} {verdict}", flush=True)

            write_lf(SRC, original)
            run(["cmake", "--build", str(build)])
    finally:
        write_lf(SRC, original)
        run(["cmake", "--build", str(build)])

    print(f"\ncaught {passed}, missed {failed}"
          + (f" -- {retries} verify retry(s) after a harness hang" if retries else ""))
    if failed:
        print("A mutation slipped through: the corpus does not reach that path.", file=sys.stderr)
        return 1
    print("All mutations caught.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
