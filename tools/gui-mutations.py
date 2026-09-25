#!/usr/bin/env python3
"""Prove tools/verify-gui.mjs catches a generator that renders wrongly.

    python tools/gui-mutations.py <msvc-build-dir>

Run from a shell with the MSVC environment (vcvars64), since the generator
builds only with MSVC and clang-cl; see CMakeLists.txt.

verify-gui passed on its first complete run. Each mutation below breaks one
thing the generator's engine path does between a spin box and a WAV file,
rebuilds klattsch_gui, and requires the verifier to fail. The window itself
-- tab order, labels, what NVDA reads -- is not reachable from here, and is
checked by hand (docs/20-generator.md).

Every pattern must occur exactly once. Exit 0 means every mutation was caught.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GUI = ROOT / "gui-native" / "klattsch_gui.cpp"

MUTATIONS = [
    ("scale's unit is 0.1, not 0.01",
     '{ L"Formant scale (%):",        50,  200, 100, 0.01, KL_OPT_SCALE },',
     '{ L"Formant scale (%):",        50,  200, 100, 0.1, KL_OPT_SCALE },'),
    ("vibrato depth and rate reach each other's option",
     '{ L"Vibrato depth (Hz):",        0,   50,   0, 1.0,  KL_OPT_VIBRATO_DEPTH },\n'
     '    { L"Vibrato rate (Hz):",         1,   20,   5, 1.0,  KL_OPT_VIBRATO_RATE },',
     '{ L"Vibrato depth (Hz):",        0,   50,   0, 1.0,  KL_OPT_VIBRATO_RATE },\n'
     '    { L"Vibrato rate (Hz):",         1,   20,   5, 1.0,  KL_OPT_VIBRATO_DEPTH },'),
    ("no option is marked present",
     "        opts.present |= 1u << PARAMS[k].opt;\n",
     ""),
    ("the chosen bank is ignored",
     "    opts.bank = kl_banks[v.bank].name;",
     "    opts.bank = kl_default_bank;"),
    ("the sample rate is always 48000",
     "    double sr = (double)v.sampleRate;",
     "    double sr = 48000.0;"),
    ("the contour is sized for 120 Hz whatever the pitch",
     "    o.base_f0 = v.value[0] * PARAMS[0].unit;",
     "    o.base_f0 = 0.0;"),
    ("phoneme mode is inverted",
     "    std::string source = phonemeMode ? utf8 : TextToSource(utf8, v);",
     "    std::string source = phonemeMode ? TextToSource(utf8, v) : utf8;"),
    ("the source is not written into the WAV",
     "    meta.comment = source.c_str();",
     "    meta.comment = NULL;"),
    ("no peak normalization",
     "KL_WAV_PEAK_NORMALIZE, &meta, r.wav.data(), cap, NULL);",
     "0.0, &meta, r.wav.data(), cap, NULL);"),
    ("the self-test reads the settings one argument early",
     "        v.value[k] = _wtoi(argv[6 + k]);",
     "        v.value[k] = _wtoi(argv[5 + k]);"),
]


def run(cmd, timeout=600):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout)
    return r.returncode


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build-msvc")
    gui = ROOT / build / "klattsch_gui.exe"
    dump = ROOT / build / "kl_text_dump.exe"
    if not gui.exists() or not dump.exists():
        print(f"no klattsch_gui.exe / kl_text_dump.exe in {build}", file=sys.stderr)
        return 2
    if run(["git", "diff", "--quiet", "--", str(GUI.relative_to(ROOT))]) != 0:
        print("refusing to run: gui-native/klattsch_gui.cpp has uncommitted changes",
              file=sys.stderr)
        return 2

    original = GUI.read_text(encoding="utf-8")
    verify = ["node", "tools/verify-gui.mjs", str(gui), str(dump)]
    caught = missed = 0
    print("Breaking the generator's engine path; each line must be caught.\n")
    try:
        for label, find, repl in MUTATIONS:
            if original.count(find) != 1:
                print(f"  {label:<54} SKIP (pattern matches {original.count(find)} times)")
                missed += 1
                continue
            GUI.write_bytes(original.replace(find, repl, 1).encode("utf-8"))
            if run(["cmake", "--build", str(build), "--target", "klattsch_gui"]) != 0:
                verdict, ok = "caught (did not compile)", True
            elif run(verify) != 0:
                verdict, ok = "caught", True
            else:
                verdict, ok = "NOT CAUGHT  <-- gap", False
            caught += ok
            missed += not ok
            print(f"  {label:<54} {verdict}", flush=True)
    finally:
        GUI.write_bytes(original.encode("utf-8"))
        run(["cmake", "--build", str(build), "--target", "klattsch_gui"])

    print(f"\n{caught} caught, {missed} not")
    return 0 if missed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
