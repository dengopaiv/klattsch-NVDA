#!/usr/bin/env python3
"""Package the NVDA add-on: dist/klattschNative-<version>-<stage>.nvda-addon.

    python packaging/make-addon.py --tested-on 2026.3.0 [--pretest] [--build build-msvc]

Run from the repo root after an MSVC Release build (the add-on's library
ships from MSVC, like the generator):

    cmake -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release   (vcvars64 shell)
    cmake --build build-msvc

Before it packs anything it checks, and refuses on any failure:

  - the version is csrc/kl_version.h's, and its spellings agree;
  - the working tree is clean, and the commit goes into the readme;
  - the library is x64, and kl_api_version() in the very file being packed
    says the same version (loaded with ctypes, so this needs 64-bit Python);
  - kl_api_test passes (the API's exit test, on this build's sources), and
    tools/test-driver.py passes against the very library being packed.

--tested-on is the NVDA version the hands-on test of step 6 ran on, and
becomes lastTestedNVDAVersion: the manifest says what was tested and nothing
newer. Before that test has passed, build with --pretest: the file name and
the readme then say so, and the package is for that test only.

What goes in: manifest.ini, synthDrivers/klattsch.py and klattschNative.dll,
doc/en/readme.html, LICENSE.txt and NOTICE.md. MIT for the engine and
BSD-3-Clause for the front end both require their notices to travel with a
binary. dist/ is ignored by git; a beta goes to testers, not to a release.
"""

import ctypes
import re
import subprocess
import sys
import zipfile
from pathlib import Path

from kl_release import DIST, ROOT, fail, require_clean, version, write_sha256

PROG = "make-addon"
ADDON = ROOT / "nvda-addon"


def arg(name, default=None):
    if name in sys.argv:
        i = sys.argv.index(name)
        if i + 1 >= len(sys.argv):
            fail(PROG, f"{name} needs a value")
        return sys.argv[i + 1]
    return default


def pe_machine(path):
    """The COFF machine field of a PE file: 0x8664 is x64."""
    data = path.read_bytes()
    pe = int.from_bytes(data[0x3C:0x40], "little")
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    return int.from_bytes(data[pe + 4:pe + 6], "little")


def run(cmd, what):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    if r.returncode != 0:
        fail(PROG, f"{what} failed:\n{r.stdout}{r.stderr}")
    return r.stdout.strip().splitlines()[-1]


def main():
    tested = arg("--tested-on")
    if not tested or not re.fullmatch(r"\d{4}\.\d+\.\d+", tested):
        fail(PROG, "--tested-on YEAR.MAJOR.MINOR is required: the NVDA version the "
                   "hands-on test ran on (or is about to run on, with --pretest)")
    pretest = "--pretest" in sys.argv
    build = ROOT / arg("--build", "build-msvc")
    lib = build / "klattschNative.dll"
    api_test = build / "kl_api_test.exe"
    if not lib.exists() or not api_test.exists():
        fail(PROG, f"no klattschNative.dll and kl_api_test.exe in {build}; build first")

    ver, stage = version(PROG)
    commit = require_clean(PROG)

    if pe_machine(lib) != 0x8664:
        fail(PROG, f"{lib.name} is not x64; no 32-bit build of anything is made here")
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        fail(PROG, "run this with 64-bit Python: it loads the library to read its version")
    dll = ctypes.CDLL(str(lib))
    dll.kl_api_version.restype = ctypes.c_char_p
    lib_ver = dll.kl_api_version().decode("ascii")
    if lib_ver != ver:
        fail(PROG, f"{lib.name} says {lib_ver!r}, csrc/kl_version.h says {ver!r}: rebuild")

    print("verifying what is being packed...")
    print("  " + run([str(api_test), "stream", str(ROOT / "goldens" / "cases-compile.bin")],
                     "kl_api_test stream"))
    print("  " + run([str(api_test), "api", str(ROOT / "tools" / "text-corpus.txt")],
                     "kl_api_test api"))
    print("  driver: " + run([sys.executable, "-I", str(ROOT / "tools" / "test-driver.py"), str(lib)],
                             "tools/test-driver.py"))

    addon_version = f"{ver}-{stage}"
    status = (f"A test build for the first hands-on test, on NVDA {tested}; not yet tested."
              if pretest else f"A {stage} for testers. Tested on NVDA {tested}.")
    manifest = (ADDON / "manifest.ini.in").read_text(encoding="utf-8")
    manifest = manifest.replace("{VERSION}", addon_version).replace("{LAST_TESTED}", tested)
    readme = (ADDON / "addon" / "doc" / "en" / "readme.html").read_text(encoding="utf-8")
    readme = (readme.replace("{VERSION}", f"{ver} {stage}")
                    .replace("{STATUS}", status)
                    .replace("{COMMIT}", commit))
    for text, name in ((manifest, "manifest"), (readme, "readme")):
        left = re.findall(r"\{[A-Z_]+\}", text)
        if left:
            fail(PROG, f"the {name} still has placeholders: {left}")

    DIST.mkdir(exist_ok=True)
    out = DIST / f"klattschNative-{addon_version}{'-pretest' if pretest else ''}.nvda-addon"
    if out.exists():
        out.unlink()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.ini", manifest.replace("\n", "\r\n").encode("utf-8"))
        z.write(ADDON / "addon" / "synthDrivers" / "klattsch.py", "synthDrivers/klattsch.py")
        z.write(lib, "synthDrivers/klattschNative.dll")
        z.writestr("doc/en/readme.html", readme.encode("utf-8"))
        z.write(ROOT / "LICENSE", "LICENSE.txt")
        z.write(ROOT / "NOTICE.md", "NOTICE.md")

    digest = write_sha256(out)
    print(f"wrote {out.relative_to(ROOT)}  ({out.stat().st_size:,} bytes)")
    print(f"sha256 {digest}")
    print(f"version {addon_version}, lastTestedNVDAVersion {tested}, from commit {commit}")


if __name__ == "__main__":
    main()
