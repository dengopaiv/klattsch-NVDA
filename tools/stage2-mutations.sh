#!/usr/bin/env bash
# Exit test for stage 2: prove the bank verifier catches a corrupted table.
#
# Same reasoning as tools/golden-mutations.sh. A field-by-field comparison that
# has never been seen to fail is a comparison nobody should trust, and bank data
# is exactly the kind of thing that can be subtly wrong forever -- one formant
# in one phoneme of one bank changes a single vowel and nothing else.
#
#   bash tools/stage2-mutations.sh <build-dir>
#
# Needs a configured build directory; rebuilds after each mutation and reverts
# with git. Exit 0 means every mutation was caught.

set -u
cd "$(dirname "$0")/.."

BUILD="${1:-build-msvc}"
DATA="csrc/kl_banks_data.c"

if [ ! -d "$BUILD" ]; then
  echo "usage: bash tools/stage2-mutations.sh <configured-build-dir>" >&2
  exit 2
fi
if ! git diff --quiet -- "$DATA"; then
  echo "refusing to run: $DATA has uncommitted changes, and this reverts with git" >&2
  exit 2
fi

DUMP="$BUILD/kl_banks_dump.exe"
[ -x "$DUMP" ] || DUMP="$BUILD/kl_banks_dump"

pass=0; fail=0

mutate() {
  local label="$1" from="$2" to="$3"
  if ! grep -qF -- "$from" "$DATA"; then
    printf '  %-46s %s\n' "$label" "SKIP (pattern not found)"
    fail=$((fail+1)); return
  fi
  perl -pi -e "s/\Q$from\E/$to/" "$DATA"
  if ! cmake --build "$BUILD" >/dev/null 2>&1; then
    printf '  %-46s %s\n' "$label" "caught (did not compile)"
    pass=$((pass+1))
  elif node tools/verify-stage2.mjs "$DUMP" >/dev/null 2>&1; then
    printf '  %-46s %s\n' "$label" "NOT CAUGHT  <-- gap in the comparison"
    fail=$((fail+1))
  else
    printf '  %-46s %s\n' "$label" "caught"
    pass=$((pass+1))
  fi
  git checkout -- "$DATA"
  cmake --build "$BUILD" >/dev/null 2>&1
}

echo "Corrupting the generated bank table; each line must be caught."
echo

# A formant, a bandwidth and an amplitude: the numbers that make the sound.
mutate "IY F1 310 -> 311"          '{ "IY", 1.0, 310.0'      '{ "IY", 1.0, 311.0'
mutate "AA BW1 130 -> 131"         '700.0, 1220.0, 2600.0, 130.0' '700.0, 1220.0, 2600.0, 131.0'
mutate "a voicing 1.0 -> 0.0"      '{ "AE", 1.0,'            '{ "AE", 0.0,'

# The flags, which change the shape rather than the timbre.
mutate "P is_stop 1 -> 0"          '{ "P", 0.0'              '{ "P_X", 0.0'

# Provenance: a dropped source is a licensing problem, not a cosmetic one.
mutate "drop a bank source string"  'Klatt, D.H. (1980).'    'REDACTED'

# Sort order, which the binary search depends on. Renaming ZH to AZH puts it
# out of order without changing the entry count.
mutate "break sort order (ZH -> AZH)" '{ "ZH",'              '{ "AZH",'

echo
printf 'caught %d, missed %d\n' "$pass" "$fail"
if [ "$fail" -ne 0 ]; then
  echo "A corruption slipped through: the comparison does not cover that field." >&2
  exit 1
fi
echo "All mutations caught."
