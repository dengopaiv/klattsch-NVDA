"""What every package made here checks before it packs anything.

Shared by packaging/make-dist.py (the generator beta) and
packaging/make-addon.py (the NVDA add-on), so that the version is read, and
the tree is judged clean, in one way only.
"""

import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"


def fail(prog, msg):
    print(f"{prog}: {msg}", file=sys.stderr)
    sys.exit(1)


def version(prog):
    """(KL_VERSION_STRING, KL_VERSION_STAGE) from csrc/kl_version.h, after
    checking that its three spellings agree."""
    h = (ROOT / "csrc" / "kl_version.h").read_text(encoding="utf-8")
    num = {k: re.search(rf"#define KL_VERSION_{k} (\d+)", h).group(1)
           for k in ("MAJOR", "MINOR", "PATCH")}
    string = re.search(r'#define KL_VERSION_STRING "([^"]+)"', h).group(1)
    stage = re.search(r'#define KL_VERSION_STAGE "([^"]+)"', h).group(1)
    display = re.search(r'#define KL_VERSION_DISPLAY "([^"]+)"', h).group(1)
    dotted = f"{num['MAJOR']}.{num['MINOR']}.{num['PATCH']}"
    if string != dotted or display != f"{string} {stage}":
        fail(prog, f"csrc/kl_version.h disagrees with itself: {dotted} / {string!r} / {display!r}")
    return string, stage


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                          text=True).stdout.strip()


def require_clean(prog):
    """The short commit, after refusing a tree with uncommitted changes: a
    package has to be traceable to the source it came from."""
    if git("status", "--porcelain", "--untracked-files=no"):
        fail(prog, "the working tree has uncommitted changes; package from a commit")
    return git("rev-parse", "--short", "HEAD")


def write_sha256(path):
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    (path.parent / (path.name + ".sha256")).write_text(f"{digest}  {path.name}\n", encoding="ascii")
    return digest
