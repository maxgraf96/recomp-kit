"""Find, strip and sign the Mach-O files of a staged tree.

Staging runs on the packager's Mac (Xcode or the Command Line Tools provide
strip; codesign is part of macOS). Stripping invalidates a signature, so the
tree is signed after it: ad hoc, which is what an app without a Developer ID
carries, and what lets the binaries run on Apple Silicon at all.
"""

from pathlib import Path
import struct
import subprocess

MACHO_MAGICS = {0xfeedfacf, 0xfeedface, 0xcafebabe, 0xcafebabf}


def is_macho(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        return False
    with open(path, "rb") as stream:
        head = stream.read(8)
    if len(head) < 8:
        return False
    little, big = struct.unpack("<I", head[:4])[0], struct.unpack(">I", head[:4])[0]
    if big in (0xcafebabe, 0xcafebabf):
        # A Java class file shares the universal magic; its "slice count" is a version >= 45.
        return struct.unpack(">I", head[4:8])[0] < 32
    return little in MACHO_MAGICS


def macho_files(root):
    """Every Mach-O file under `root` (symlinks excluded), sorted."""
    return sorted(p for p in Path(root).rglob("*") if is_macho(p))


def strip_local_symbols(root, files=None):
    """strip -x: drop local symbols, keep everything another image may bind to."""
    for path in files or macho_files(root):
        subprocess.run(["strip", "-x", str(path)], check=True)


def sign_ad_hoc(root, files=None):
    """Re-sign every Mach-O ad hoc (codesign -s - -f)."""
    targets = [str(p) for p in (files or macho_files(root))]
    if targets:
        subprocess.run(["codesign", "-s", "-", "-f"] + targets, check=True, capture_output=True)


def verify_signatures(root, files=None):
    """codesign --verify every Mach-O; raise naming the first that fails."""
    for path in files or macho_files(root):
        result = subprocess.run(["codesign", "--verify", "--strict", str(path)], capture_output=True, text=True)
        if result.returncode:
            raise SystemExit("macho_files: %s fails codesign --verify: %s" % (path, result.stderr.strip()))
