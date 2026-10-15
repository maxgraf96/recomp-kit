#!/usr/bin/env python3
"""Stage the bundled Python: python-build-standalone, trimmed, plus capstone and pefile.

    tools/toolchain/python_runtime.py --cache DIR --out DIR/python

A Mac without the Command Line Tools has no usable Python (/usr/bin/python3
only offers to install them), so an app that translates on first run brings
its own. This stages the pinned CPython (pins.json, python-build-standalone,
aarch64-apple-darwin, install_only) and installs the pinned capstone and
pefile wheels into it, with no pip and no network beyond the pinned files:

  * trimmed of what the translator never runs: Tcl/Tk and tkinter, IDLE,
    ensurepip and pip, lib2to3, pydoc_data, turtle, headers, man pages,
    the static-library config directory and the test extension modules;
  * every .py precompiled as an unchecked-hash .pyc, so imports never need
    to write one and the result does not depend on file times;
  * every Mach-O stripped of local symbols (strip -x) and signed ad hoc.

The result is relocatable: it runs from any path, read-only ones included
(App Translocation). Run it as

    <out>/bin/python3 -I -B script.py ...

-I ignores PYTHON* variables, the user's site-packages and the script's
directory; -B writes no bytecode beside anything it imports (the shipped
.pyc files are read, never rewritten). A sitecustomize.py makes the second
the default, for tools that start the interpreter with -I alone.
"""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import zipfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fetch  # noqa: E402
import macho_files  # noqa: E402

# Relative to the installation; X.Y is the pinned version's.
TRIM = (
    "bin/2to3", "bin/2to3-X.Y", "bin/idle3", "bin/idleX.Y", "bin/pip", "bin/pip3", "bin/pipX.Y",
    "bin/pydoc3", "bin/pydocX.Y", "bin/python", "bin/python3-config", "bin/pythonX.Y-config",
    "include", "share", "lib/pkgconfig", "lib/itcl*", "lib/libtcl*", "lib/libtk*", "lib/tcl*",
    "lib/thread*", "lib/tk*",
    "lib/pythonX.Y/config-X.Y-darwin", "lib/pythonX.Y/ensurepip", "lib/pythonX.Y/idlelib",
    "lib/pythonX.Y/lib2to3", "lib/pythonX.Y/pydoc_data", "lib/pythonX.Y/tkinter",
    "lib/pythonX.Y/turtledemo", "lib/pythonX.Y/turtle.py", "lib/pythonX.Y/test",
    "lib/pythonX.Y/site-packages/pip", "lib/pythonX.Y/site-packages/pip-*",
    "lib/pythonX.Y/lib-dynload/_tkinter.*", "lib/pythonX.Y/lib-dynload/_ctypes_test.*",
    "lib/pythonX.Y/lib-dynload/xxlimited*", "lib/pythonX.Y/lib-dynload/_test*",
    "**/__pycache__",
)
# Inside installed wheels: the C API headers capstone ships for C users.
WHEEL_TRIM = ("capstone/include",)

# site-packages/sitecustomize.py: an app's interpreter never writes bytecode,
# even when a tool starts it without -B (-I ignores PYTHONDONTWRITEBYTECODE,
# but still runs sitecustomize).
SITECUSTOMIZE = '''"""This Python is part of a signed app: it never writes bytecode beside what it
imports (recomp-kit tools/toolchain/python_runtime.py). The shipped .pyc files
are read as they are."""
import sys

sys.dont_write_bytecode = True
'''


def short_version(version):
    return ".".join(version.split(".")[:2])


def remove(root, patterns, version):
    """Delete every path under `root` matching one of `patterns` (X.Y = version)."""
    xy = short_version(version)
    for pattern in patterns:
        pattern = pattern.replace("X.Y", xy)
        for path in sorted(root.glob(pattern), reverse=True):
            if path.is_symlink() or path.is_file():
                path.unlink()
            elif path.is_dir():
                shutil.rmtree(path)


def unpack_python(tarball, out):
    """Unpack the install_only archive's python/ tree as `out`."""
    if out.exists():
        shutil.rmtree(out)
    staging = out.parent / (out.name + ".unpack")
    if staging.exists():
        shutil.rmtree(staging)
    with tarfile.open(tarball) as archive:
        archive.extractall(staging, filter="tar")
    (staging / "python").rename(out)
    staging.rmdir()


def install_wheel(wheel, site_packages):
    """Unpack a pure or platform wheel into site-packages, as an installer would.

    Only plain wheels (no .data directory, no scripts) are accepted: the
    pinned two are, and anything else is a pin change that should be looked at."""
    with zipfile.ZipFile(wheel) as archive:
        names = archive.namelist()
        if any(".data/" in name for name in names):
            raise SystemExit("python_runtime: %s has a .data directory; not supported" % wheel.name)
        for info in archive.infolist():
            target = site_packages / info.filename
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, open(target, "wb") as dest:
                shutil.copyfileobj(source, dest)
            mode = (info.external_attr >> 16) & 0o777
            target.chmod(0o755 if mode & 0o111 else 0o644)
        dist_info = next(n.split("/", 1)[0] for n in names if n.split("/", 1)[0].endswith(".dist-info"))
        (site_packages / dist_info / "INSTALLER").write_text("recomp-kit\n")


def compile_bytecode(python, root, *trees):
    """Precompile `trees` as unchecked-hash .pyc with the staged interpreter itself.

    The file names compiled in are relative to `root`'s parent ("python/lib/..."),
    not this machine's paths, so the files are the same wherever it was staged;
    an import replaces them with the real path anyway."""
    # -B: the interpreter's own start-up imports must not leave timestamp .pyc behind.
    subprocess.run([str(python), "-I", "-B", "-m", "compileall", "-q", "-f", "-j", "0",
                    "--invalidation-mode", "unchecked-hash", "-s", str(root), "-p", root.name]
                   + [str(t) for t in trees], check=True)


SELF_TEST = r"""
import importlib.metadata as md, sys, tomllib, capstone, pefile
assert sys.flags.isolated and sys.dont_write_bytecode, (sys.flags, sys.dont_write_bytecode)
pins = dict(arg.split("=", 1) for arg in sys.argv[1:])
for name, version in pins.items():
    assert md.version(name) == version, (name, md.version(name), version)
md_ = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
text = "; ".join("%s %s" % (i.mnemonic, i.op_str) for i in md_.disasm(b"\x55\x8b\xec\xc3", 0x1000))
assert text == "push ebp; mov ebp, esp; ret ", text
print("python %s, capstone %s, pefile %s" % (sys.version.split()[0], md.version("capstone"), md.version("pefile")))
"""


def self_test(python, wheels):
    """The staged interpreter imports the pinned packages and decodes x86, with
    -B and without it (sitecustomize then keeps it from writing bytecode)."""
    args = ["%s=%s" % (w["name"], w["version"]) for w in wheels]
    out = ""
    for flags in (["-I", "-B"], ["-I"]):
        out = subprocess.run([str(python)] + flags + ["-c", SELF_TEST] + args, check=True,
                             capture_output=True, text=True).stdout.strip()
    return out


def stage(cache, out, strip=True, sign=True):
    """Stage the trimmed runtime at `out` (the python/ directory); return its interpreter."""
    pins = fetch.load_pins()
    pin = pins["python"]
    unpack_python(fetch.fetch(pin, cache), out)
    remove(out, TRIM, pin["version"])
    site_packages = out / "lib" / ("python%s" % short_version(pin["version"])) / "site-packages"
    for wheel_pin in pins["wheels"]:
        install_wheel(fetch.fetch(wheel_pin, cache), site_packages)
    remove(site_packages, WHEEL_TRIM, pin["version"])
    (site_packages / "sitecustomize.py").write_text(SITECUSTOMIZE)
    if strip:
        macho_files.strip_local_symbols(out)
    if sign:
        macho_files.sign_ad_hoc(out)
    python = out / "bin" / "python3"
    compile_bytecode(python, out, out / "lib")
    print("python_runtime: %s" % self_test(python, pins["wheels"]), flush=True)
    return python


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache", type=Path, required=True, help="downloads")
    parser.add_argument("--out", type=Path, required=True, help="the python/ directory to create")
    args = parser.parse_args(argv)
    stage(args.cache, args.out.resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
