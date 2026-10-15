#!/usr/bin/env python3
"""Link translated modules into bundles the host loads: the freestanding build.

    tools/recomp/bundle.py --gen build/recomp/gen --out build/recomp/bundles \\
        -D GUEST_IMAGE_BASE=0x00400000u -D GUEST_HEAP_BASE=0x01000000u -D GUEST_SIZE=0x10000000u \\
        [--module main --module <aux key> ...] [--cc CLANG] [--lld LD64_LLD] [--jobs N]

Each module of a translation - the image (gen/, key "main") and every
auxiliary module (gen/aux-<key>/) - is compiled with -ffreestanding -nostdinc
and RECOMP_FREESTANDING (runtime/freestanding.h): only the compiler's own
headers, no SDK and no C library. ld64.lld links it into <out>/<key>.bundle
with -bundle -undefined dynamic_lookup, again without an SDK or libSystem
stub: every symbol a bundle does not define is a runtime entry point that the
host process supplies when it dlopen()s the bundle. The module's table
registers itself from a constructor (x86.h RecompModule), so the host links
nothing of it. lld signs arm64 output ad hoc.

Nothing here needs the Xcode Command Line Tools: an LLVM clang and lld are
enough (RECOMP_FREESTANDING_CC, RECOMP_LD64_LLD, or Homebrew's llvm and lld).
The symbol check reads the Mach-O's own symbol table (no nm).
An object is rebuilt only when its source, a header it read or the flags change.
After linking, the bundle's undefined symbols are checked against what the
host provides (FOREIGN_OK and the runtime's prefixes); anything else - a
C library function the headers let through - fails the build.
"""

import argparse
import concurrent.futures
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]

# What a bundle may leave to the host besides the runtime's own entry points:
# the C library functions every freestanding C program may need (the compiler
# emits calls to the first four for any large copy), the host's setjmp (the
# runtime longjmps to its buffers), and the stack probe Darwin's compiler
# inserts in frames over a page.
FOREIGN_OK = frozenset(("_memcpy", "_memmove", "_memset", "_memcmp", "_bzero", "___bzero",
                        "_setjmp", "___chkstk_darwin"))
# The runtime's entry points and data, as generated code names them.
RUNTIME_PREFIXES = ("_recomp_", "_g_mem", "_g_watch_", "_g_dirty", "_fx87", "_x87_")

DEFAULT_CC = ("/opt/homebrew/opt/llvm/bin/clang", "clang")
DEFAULT_LLD = ("/opt/homebrew/opt/lld/bin/ld64.lld", "ld64.lld")


def find_tool(explicit, env, defaults):
    """The named tool, the environment's, or the first default that exists."""
    for candidate in (explicit, os.environ.get(env)) + tuple(defaults):
        if not candidate:
            continue
        path = shutil.which(candidate) if os.sep not in candidate else candidate
        if path and Path(path).is_file():
            return path
    raise SystemExit("bundle.py: no %s; install LLVM (brew install llvm lld) or set %s"
                     % (defaults[-1], env))


def modules_in(gen):
    """{key: source directory} for the translation in `gen`."""
    found = {}
    if (gen / "table.c").is_file():
        found["main"] = gen
    for d in sorted(gen.glob("aux-*")):
        if (d / "table.c").is_file():
            found[d.name[len("aux-"):]] = d
    return found


def compile_flags(cc, gen, source_dir, defines, min_macos):
    """The freestanding compile, minus input and output."""
    resource = subprocess.check_output([cc, "-print-resource-dir"], text=True).strip()
    flags = ["-target", "arm64-apple-macos%s" % min_macos, "-std=c11", "-O2",
             "-ffreestanding", "-nostdinc", "-isystem", os.path.join(resource, "include"),
             # The libm builtins in freestanding.h stay single instructions;
             # no contraction, as ADR-level determinism demands of the image.
             "-fno-math-errno", "-ffp-contract=off",
             "-DRECOMP_FREESTANDING=1"]
    flags += ["-D" + d for d in defines]
    flags += ["-I", str(source_dir), "-I", str(gen), "-I", str(ROOT), "-I", str(ROOT / "runtime"),
              "-Wall", "-Wextra", "-Wno-unused"]
    return flags


def stale(obj, dep, stamp, flags_hash):
    """Does `obj` need compiling? Its depfile lists every header it read."""
    if not obj.is_file() or not dep.is_file() or not stamp.is_file():
        return True
    if stamp.read_text() != flags_hash:
        return True
    built = obj.stat().st_mtime
    text = dep.read_text().replace("\\\n", " ")
    inputs = text.split(":", 1)[1].split() if ":" in text else []
    return any(not Path(p).is_file() or Path(p).stat().st_mtime > built for p in inputs)


def compile_one(cc, flags, flags_hash, source, obj):
    dep, stamp = obj.with_suffix(".d"), obj.with_suffix(".flags")
    if not stale(obj, dep, stamp, flags_hash):
        return False
    subprocess.run([cc] + flags + ["-MD", "-MF", str(dep), "-c", str(source), "-o", str(obj)],
                   check=True)
    stamp.write_text(flags_hash)
    return True


MH_MAGIC_64 = 0xfeedfacf
LC_SYMTAB = 0x2
N_STAB, N_TYPE, N_UNDF = 0xe0, 0x0e, 0x0


def macho_undefined(path):
    """The undefined symbols of a thin 64-bit Mach-O (an object or a bundle), as
    `nm -u` lists them, read from its LC_SYMTAB: no nm is needed, and on a Mac
    without the Command Line Tools /usr/bin/nm is only a stub that offers to
    install them. None when path is not such a file."""
    data = Path(path).read_bytes()
    if len(data) < 32 or struct.unpack_from("<I", data, 0)[0] != MH_MAGIC_64:
        return None
    ncmds = struct.unpack_from("<I", data, 16)[0]
    off = 32
    for _ in range(ncmds):
        if off + 8 > len(data):
            return None
        cmd, size = struct.unpack_from("<II", data, off)
        if size < 8 or off + size > len(data):
            return None
        if cmd == LC_SYMTAB:
            if size < 24:
                return None
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", data, off + 8)
            if symoff + 16 * nsyms > len(data) or stroff + strsize > len(data):
                return None
            out = set()
            for i in range(nsyms):
                strx, ntype, _, _, value = struct.unpack_from("<IBBHQ", data, symoff + 16 * i)
                # Not a debugging entry, undefined, and not a common symbol (value 0).
                if ntype & N_STAB or ntype & N_TYPE != N_UNDF or value != 0:
                    continue
                start = stroff + strx
                end = data.find(b"\0", start, stroff + strsize)
                out.add(data[start:end if end >= 0 else stroff + strsize].decode("utf-8", "replace"))
            return sorted(out)
        off += size
    return []


def undefined_symbols(bundle, cc):
    """The bundle's undefined symbols: read from the Mach-O itself, else via
    llvm-nm beside the compiler or nm."""
    found = macho_undefined(bundle)
    if found is not None:
        return found
    nm = Path(cc).with_name("llvm-nm")
    tool = str(nm) if nm.is_file() else (shutil.which("llvm-nm") or shutil.which("nm"))
    if tool is None:
        raise SystemExit("bundle.py: no llvm-nm or nm to check %s" % bundle)
    out = subprocess.check_output([tool, "-u", str(bundle)], text=True)
    return sorted({line.split()[-1] for line in out.splitlines() if line.strip()})


def foreign(symbols):
    """The undefined symbols the host does not provide."""
    return [s for s in symbols if s not in FOREIGN_OK and not s.startswith(RUNTIME_PREFIXES)]


def build(gen, out, keys, cc, lld, defines, jobs, min_macos):
    """Compile and link each module; return {key: bundle path}."""
    available = modules_in(gen)
    keys = keys or list(available)
    missing = [k for k in keys if k not in available]
    if missing:
        raise SystemExit("bundle.py: %s has no translation for %s" % (gen, ", ".join(missing)))
    out.mkdir(parents=True, exist_ok=True)
    bundles = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        # Every module's objects share the pool, so a small module does not
        # wait for a large one; each links as soon as its own objects are done.
        plans = []
        for key in keys:
            source_dir = available[key]
            flags = compile_flags(cc, gen, source_dir, defines, min_macos)
            flags_hash = hashlib.sha256(("\0".join([cc] + flags)).encode()).hexdigest()
            objdir = out / ("%s.objects" % key)
            objdir.mkdir(exist_ok=True)
            sources = sorted(source_dir.glob("chunk_*.c")) + [source_dir / "table.c"]
            work = [pool.submit(compile_one, cc, flags, flags_hash, s, objdir / (s.stem + ".o"))
                    for s in sources]
            plans.append((key, objdir, sources, work))
        for key, objdir, sources, work in plans:
            rebuilt = sum(1 for w in work if w.result())
            bundle = out / ("%s.bundle" % key)
            objects = [str(objdir / (s.stem + ".o")) for s in sources]
            if rebuilt or not bundle.is_file():
                subprocess.run([lld, "-arch", "arm64", "-platform_version", "macos", min_macos, min_macos,
                                "-bundle", "-undefined", "dynamic_lookup", "-o", str(bundle)] + objects,
                               check=True)
            symbols = undefined_symbols(bundle, cc)
            (out / ("%s.undefined" % key)).write_text("\n".join(symbols) + "\n")
            bad = foreign(symbols)
            if bad:
                raise SystemExit("bundle.py: %s needs %s from the C library; route it through the "
                                 "runtime (runtime/freestanding.h)" % (bundle, ", ".join(bad)))
            print("bundle %s: %d objects (%d rebuilt), %d host symbols -> %s"
                  % (key, len(objects), rebuilt, len(symbols), bundle), flush=True)
            bundles[key] = bundle
    return bundles


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--gen", type=Path, required=True, help="the translation (build/recomp/gen)")
    parser.add_argument("--out", type=Path, required=True, help="where <key>.bundle goes")
    parser.add_argument("--module", action="append", default=[],
                        help='"main" for the image or an auxiliary module key; default: every module')
    parser.add_argument("-D", dest="defines", action="append", default=[],
                        help="NAME=VALUE for the compile: GUEST_IMAGE_BASE, GUEST_HEAP_BASE, GUEST_SIZE")
    parser.add_argument("--cc", default=None, help="an LLVM clang (default $RECOMP_FREESTANDING_CC, "
                        "then Homebrew's, then PATH)")
    parser.add_argument("--lld", default=None, help="ld64.lld (default $RECOMP_LD64_LLD, then "
                        "Homebrew's, then PATH)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--min-macos", default="13.0")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"\d+\.\d+", args.min_macos):
        parser.error("--min-macos must be major.minor")
    cc = find_tool(args.cc, "RECOMP_FREESTANDING_CC", DEFAULT_CC)
    lld = find_tool(args.lld, "RECOMP_LD64_LLD", DEFAULT_LLD)
    try:
        build(args.gen.resolve(), args.out.resolve(), args.module, cc, lld, args.defines,
              max(1, args.jobs), args.min_macos)
    except subprocess.CalledProcessError as error:
        parser.exit(1, "bundle.py: %s failed\n" % " ".join(error.cmd[:3]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
