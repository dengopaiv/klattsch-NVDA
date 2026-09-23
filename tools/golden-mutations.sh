#!/usr/bin/env bash
# Exit test for stage 0: prove the goldens catch a broken engine.
#
# A golden corpus that cannot fail is worse than no corpus, because the one
# time it matters nobody looks. This mutates the engine one constant at a time,
# in every subsystem the port touches, and requires `goldens.mjs --check` to
# fail on each. Every mutation is reverted with git before the next runs.
#
#   bash tools/golden-mutations.sh
#
# Exit 0 means every mutation was caught.

set -u
cd "$(dirname "$0")/.."

if ! git diff --quiet -- src/ ; then
  echo "refusing to run: src/ has uncommitted changes, and this script reverts with git" >&2
  exit 2
fi

pass=0; fail=0

mutate() {
  local label="$1" file="$2" from="$3" to="$4"
  if ! grep -qF -- "$from" "$file"; then
    printf '  %-46s %s\n' "$label" "SKIP (pattern not found)"
    fail=$((fail+1)); return
  fi
  perl -pi -e "s/\Q$from\E/$to/" "$file"
  if node tools/goldens.mjs --check >/dev/null 2>&1; then
    printf '  %-46s %s\n' "$label" "NOT CAUGHT  <-- gap in the corpus"
    fail=$((fail+1))
  else
    printf '  %-46s %s\n' "$label" "caught"
    pass=$((pass+1))
  fi
  git checkout -- "$file"
}

echo "Mutating the engine; each line must be caught."
echo

echo "dsp.js -- the primitives"
mutate "softClip threshold 0.85 -> 0.849"      src/engine/dsp.js "const T = 0.85;" "const T = 0.849;"
mutate "glottal NORM 0.1 -> 0.1000001"         src/engine/dsp.js "const NORM = 0.1;" "const NORM = 0.1000001;"
mutate "glottal Tp slope 0.2 -> 0.2001"        src/engine/dsp.js "0.5 - e * 0.2" "0.5 - e * 0.2001"
mutate "glottal Tn slope 0.17 -> 0.1701"       src/engine/dsp.js "0.25 - e * 0.17" "0.25 - e * 0.1701"
mutate "xorshift shift 13 -> 14"               src/engine/dsp.js "x ^= x << 13;" "x ^= x << 14;"
mutate "xorshift >>> 17 -> >> 17 (signed)"     src/engine/dsp.js "x ^= x >>> 17;" "x ^= x >> 17;"
mutate "biquad freq clamp 40 -> 41"            src/engine/dsp.js "Math.max(40, Math.min" "Math.max(41, Math.min"
mutate "biquad bw clamp 20 -> 21"              src/engine/dsp.js "bw = Math.max(20, bw);" "bw = Math.max(21, bw);"
mutate "biquad nyquist 0.45 -> 0.451"          src/engine/dsp.js "sr * 0.45, f" "sr * 0.451, f"

echo
echo "synth-core.js -- the sample loop"
mutate "voicedGain 0.85 -> 0.851"              src/engine/synth-core.js "cur.aspiration * 0.85" "cur.aspiration * 0.851"
mutate "unvoiced noise 0.35 -> 0.351"          src/engine/synth-core.js "noiseSample * 0.35" "noiseSample * 0.351"
mutate "aspiration noise 0.5 -> 0.501"         src/engine/synth-core.js "noiseSample * 0.5;" "noiseSample * 0.501;"
mutate "default gain 3.5 -> 3.51"              src/engine/synth-core.js "gain: 3.5," "gain: 3.51,"
mutate "default F1 500 -> 501"                 src/engine/synth-core.js "F1: 500, BW1: 80" "F1: 501, BW1: 80"
mutate "LFSR seed"                             src/engine/synth-core.js "0xACE1ACE1 | 0" "0xACE1ACE2 | 0"
mutate "tremolo unipolar -> bipolar"           src/engine/synth-core.js "(0.5 + 0.5 * Math.sin" "(0.0 + 1.0 * Math.sin"

echo
echo "sequencer.js -- the compiler"
mutate "stressDurationFactor 1.5 -> 1.51"      src/engine/sequencer.js "stressDurationFactor: 1.5," "stressDurationFactor: 1.51,"
mutate "stressF0Lift 8 -> 9"                   src/engine/sequencer.js "stressF0Lift: 8," "stressF0Lift: 9,"
mutate "stopBurstMs 25 -> 26"                  src/engine/sequencer.js "stopBurstMs: 25," "stopBurstMs: 26,"
mutate "defaultTransitionMs 35 -> 36"          src/engine/sequencer.js "defaultTransitionMs: 35," "defaultTransitionMs: 36,"
mutate "fadeOutMs 100 -> 101"                  src/engine/sequencer.js "fadeOutMs: 100," "fadeOutMs: 101,"
mutate "trailOffMs 150 -> 151"                 src/engine/sequencer.js "trailOffMs: 150," "trailOffMs: 151,"
mutate "baseF0 120 -> 121"                     src/engine/sequencer.js "baseF0: 120," "baseF0: 121,"
mutate "rate 110 -> 111"                       src/engine/sequencer.js "rate: 110, " "rate: 111, "
mutate "pause comma 100 -> 101"                src/engine/sequencer.js "',': 100" "',': 101"
mutate "glide onset 0.25 -> 0.26"              src/engine/sequencer.js "slotMs * 0.25, glide = slotMs * 0.50" "slotMs * 0.26, glide = slotMs * 0.50"
mutate "stop burst fraction 0.3 -> 0.31"       src/engine/sequencer.js "slotMs * 0.3)" "slotMs * 0.31)"
mutate "steady transition 0.4 -> 0.41"         src/engine/sequencer.js "slotMs * 0.4)" "slotMs * 0.41)"
mutate "pitch-move ramp 0.6 -> 0.61"           src/engine/sequencer.js "endF0), slotMs * 0.6" "endF0), slotMs * 0.61"
mutate "A440 -> A441"                          src/engine/sequencer.js "return 440 * Math.pow" "return 441 * Math.pow"
mutate "sticky/transient swap"                 src/engine/sequencer.js "if (!t.transient) f0 += t.pitchDelta;" "if (t.transient) f0 += t.pitchDelta;"

echo
echo "banks -- the data"
# The engine imports bundled.js, not the JSON, so only bundled.js can change
# what the engine does. The goldens are the right guard for that.
mutate "bundled.js IY F1 310 -> 311"           src/engine/banks/bundled.js '"F1": 310,' '"F1": 311,'

# The JSON is the generator's source, not the engine's. A change there cannot
# move a golden and must not be expected to -- build-banks.js --check is what
# catches it. The stage 0 mutation run found this by expecting the goldens to
# catch it, which was the wrong expectation, so the division of labour is
# asserted here rather than assumed.
echo
echo "banks -- the JSON is a different guard's job"
perl -pi -e 's/\Q"F1": 310,\E/"F1": 311,/' src/engine/banks/klatt1980-en.json
if node tools/goldens.mjs --check >/dev/null 2>&1; then
  printf '  %-46s %s
' "JSON edit invisible to goldens (correct)" "as expected"
  pass=$((pass+1))
else
  printf '  %-46s %s
' "JSON edit invisible to goldens (correct)" "UNEXPECTED: goldens moved"
  fail=$((fail+1))
fi
if node tools/build-banks.js --check >/dev/null 2>&1; then
  printf '  %-46s %s
' "JSON edit caught by build-banks --check" "NOT CAUGHT  <-- guard broken"
  fail=$((fail+1))
else
  printf '  %-46s %s
' "JSON edit caught by build-banks --check" "caught"
  pass=$((pass+1))
fi
git checkout -- src/engine/banks/klatt1980-en.json

echo
printf 'caught %d, missed %d\n' "$pass" "$fail"
if [ "$fail" -ne 0 ]; then
  echo "A mutation slipped through. Either the corpus does not reach that code," >&2
  echo "or the digest does not cover that field. Both are corpus bugs." >&2
  exit 1
fi
echo "All mutations caught."
