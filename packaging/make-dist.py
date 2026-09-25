#!/usr/bin/env python3
"""Package the beta of Klattsch Native into one 7z for testers.

    python packaging/make-dist.py [--build build-msvc]

Run from the repo root after an MSVC Release build of klattsch_gui (the only
toolchain the generator ships from; see docs/20-generator.md):

    cmake -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release   (vcvars64 shell)
    cmake --build build-msvc

The archive lands at dist/klattsch-native-<version>-beta-x64.7z, with a
.sha256 beside it. dist/ is ignored by git: a beta goes to testers, not into
the public repository and not to a GitHub release.

Before it packs anything it checks, and refuses on any failure:

  - the version is the one in csrc/kl_version.h, and its three spellings
    there agree;
  - the executable's own VERSIONINFO says the same version, so the file a
    tester reports is the file that was packed;
  - the executable's render path passes tools/verify-gui.mjs (byte-identical
    to the JavaScript reference), run on this very file;
  - the working tree is clean, and the commit is recorded in the archive, so
    a report can be traced to the source it came from.

What goes in: the program, a readme for testers, klattsch's LICENSE and
NOTICE.md. Both licences require their notices to travel with a binary --
MIT for the engine, BSD-3-Clause for the text front end.
"""

import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"
SEVEN_ZIP = [r"C:\Program Files\7-Zip\7z.exe", "7z", "7za"]


def fail(msg):
    print(f"make-dist: {msg}", file=sys.stderr)
    sys.exit(1)


def version():
    h = (ROOT / "csrc" / "kl_version.h").read_text(encoding="utf-8")
    num = {k: re.search(rf"#define KL_VERSION_{k} (\d+)", h).group(1)
           for k in ("MAJOR", "MINOR", "PATCH")}
    string = re.search(r'#define KL_VERSION_STRING "([^"]+)"', h).group(1)
    stage = re.search(r'#define KL_VERSION_STAGE "([^"]+)"', h).group(1)
    display = re.search(r'#define KL_VERSION_DISPLAY "([^"]+)"', h).group(1)
    dotted = f"{num['MAJOR']}.{num['MINOR']}.{num['PATCH']}"
    if string != dotted or display != f"{string} {stage}":
        fail(f"csrc/kl_version.h disagrees with itself: {dotted} / {string!r} / {display!r}")
    return string, stage


def exe_version(exe):
    """The FileVersion string in the executable's VERSIONINFO, read raw: it is
    stored as UTF-16 after the key, which is enough to find without an API."""
    data = exe.read_bytes()
    key = "FileVersion".encode("utf-16-le")
    i = data.find(key)
    if i < 0:
        return None
    j = i + len(key)
    while data[j:j + 2] == b"\0\0":
        j += 2
    end = data.find(b"\0\0", j)
    while (end - j) % 2:
        end = data.find(b"\0\0", end + 1)
    return data[j:end].decode("utf-16-le")


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                          text=True).stdout.strip()


def main():
    build = ROOT / "build-msvc"
    if "--build" in sys.argv:
        build = ROOT / sys.argv[sys.argv.index("--build") + 1]
    exe = build / "klattsch_gui.exe"
    dump = build / "kl_text_dump.exe"
    if not exe.exists() or not dump.exists():
        fail(f"no klattsch_gui.exe and kl_text_dump.exe in {build}; build first")

    ver, stage = version()
    if exe_version(exe) != ver:
        fail(f"{exe.name} says version {exe_version(exe)!r}, csrc/kl_version.h says "
             f"{ver!r}: rebuild")
    if git("status", "--porcelain", "--untracked-files=no"):
        fail("the working tree has uncommitted changes; package from a commit")
    commit = git("rev-parse", "--short", "HEAD")

    print("verifying the executable being packed...")
    r = subprocess.run(["node", "tools/verify-gui.mjs", str(exe), str(dump)],
                       cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
    if r.returncode != 0:
        fail("tools/verify-gui.mjs failed on this executable:\n" + r.stdout + r.stderr)
    print("  " + r.stdout.strip().splitlines()[-1])

    seven = next((s for s in SEVEN_ZIP if shutil.which(s) or Path(s).exists()), None)
    if seven is None:
        fail("7-Zip not found")

    name = f"klattsch-native-{ver}-{stage}-x64"
    DIST.mkdir(exist_ok=True)
    out = DIST / f"{name}.7z"
    if out.exists():
        out.unlink()

    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / name
        folder.mkdir()
        shutil.copy2(exe, folder / "klattsch_gui.exe")
        shutil.copy2(ROOT / "LICENSE", folder / "LICENSE.txt")
        shutil.copy2(ROOT / "NOTICE.md", folder / "NOTICE.md")
        readme = (ROOT / "packaging" / "readme-beta.txt").read_text(encoding="utf-8")
        readme = readme.replace("{VERSION}", ver)
        readme += f"\nBuilt from commit {commit}.\n"
        (folder / "README.txt").write_bytes(readme.replace("\n", "\r\n").encode("utf-8"))

        r = subprocess.run([seven, "a", "-t7z", "-mx=9", str(out), name],
                           cwd=tmp, capture_output=True, text=True)
        if r.returncode != 0:
            fail("7-Zip failed:\n" + r.stdout + r.stderr)

    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    (DIST / f"{name}.7z.sha256").write_text(f"{digest}  {out.name}\n", encoding="ascii")
    print(f"wrote {out.relative_to(ROOT)}  ({out.stat().st_size:,} bytes)")
    print(f"sha256 {digest}")
    print(f"from commit {commit}")


if __name__ == "__main__":
    main()
