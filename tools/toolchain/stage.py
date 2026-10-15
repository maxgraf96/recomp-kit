#!/usr/bin/env python3
"""Stage the first-run toolchain an app bundles: Python, clang and ld64.lld, notices.

    tools/toolchain/stage.py --out DIR [--cache DIR] [--work DIR] [--jobs N] [--nice N]
                             [--profile FILE] [--no-pgo] [--keep-build] [--no-host-notices]
                             [--notices-extra JSON ...]

Everything comes from pinned upstream artifacts (pins.json), verified by
SHA-256, and is built, trimmed, stripped and signed ad hoc on this Mac (the
packager's, with Xcode or the Command Line Tools). The result:

    DIR/python/bin/python3            CPython + capstone + pefile (python_runtime.py)
    DIR/llvm/bin/llvm                 clang and lld, AArch64 only, one binary (llvm.py)
    DIR/llvm/bin/clang -> llvm
    DIR/llvm/bin/ld64.lld -> llvm
    DIR/llvm/lib/clang/<N>/include/   the compiler headers a freestanding compile reads
    DIR/THIRD_PARTY_NOTICES.txt       every licence text the app must carry (notices.py)
    DIR/MANIFEST.json                 SHA-256, size and mode of every file; the pins

The toolchain is relocatable: copy DIR anywhere (an app's Contents/Resources)
and it runs from there, read-only paths included. Use it as

    DIR/python/bin/python3 -I -B tools/recomp/bundle.py ... \\
        --cc DIR/llvm/bin/clang --lld DIR/llvm/bin/ld64.lld

LLVM is built from source with profile-guided optimisation (pgo.py: an
instrumented build compiles a corpus made from the translator's synthetic
vocabulary, then the optimised build uses that profile). --profile reuses a
merged profile; --no-pgo skips it (about a quarter slower compiles). The
build trees under --work are deleted once staged unless --keep-build; the
downloads under --cache are kept.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fetch  # noqa: E402
import llvm  # noqa: E402
import macho_files  # noqa: E402
import notices  # noqa: E402
import pgo  # noqa: E402
import python_runtime  # noqa: E402

DEFAULT_CACHE = Path.home() / "Library" / "Caches" / "recomp-kit" / "toolchain"
# System libraries every macOS has; anything else a staged binary loads must be in the tree.
SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")


def log(message):
    print("stage: %s" % message, flush=True)


def build_llvm(cache, work, jobs, nice, use_pgo, profile, keep_build):
    """Build the toolchain from source; return the build directory to stage from."""
    pins = fetch.load_pins()
    source = llvm.unpack_source(fetch.fetch(pins["llvm"]["source"], cache), work)
    final = work / "build-pgo"
    if use_pgo and profile is None:
        instr = work / "build-instr"
        log("LLVM, instrumented build (PGO stage 1)")
        llvm.build(source, instr, jobs, nice, instrument=True)
        profiles = instr / "profiles"
        if profiles.exists():
            shutil.rmtree(profiles)
        log("LLVM, training on the synthetic corpus")
        sources = pgo.corpus(work / "pgo" / "corpus")
        pgo.train(instr / "bin" / "clang", instr / "bin" / "ld64.lld", sources, work / "pgo" / "objects", jobs)
        profile = pgo.merge(profiles, work / "pgo" / "llvm.profdata")
        if not keep_build:
            shutil.rmtree(instr)
            shutil.rmtree(work / "pgo" / "objects")
    if not use_pgo:
        final = work / "build-thinlto"
        profile = None
    log("LLVM, %s build" % ("ThinLTO + PGO" if profile else "ThinLTO"))
    llvm.build(source, final, jobs, nice, profile=profile)
    return final


def manifest(out):
    """{"files": {path: {sha256, size, mode} or {symlink}}} of everything under `out`."""
    files = {}
    for path in sorted(out.rglob("*")):
        rel = path.relative_to(out).as_posix()
        if rel == "MANIFEST.json":
            continue
        if path.is_symlink():
            files[rel] = {"symlink": os.readlink(path)}
        elif path.is_file():
            data = path.read_bytes()
            files[rel] = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
                          "mode": "%o" % (path.stat().st_mode & 0o777)}
    return files


def check_links(out):
    """Every Mach-O loads only system libraries or files inside `out`."""
    for path in macho_files.macho_files(out):
        listing = subprocess.check_output(["otool", "-L", str(path)], text=True).splitlines()[1:]
        for line in listing:
            name = line.strip().split(" (", 1)[0]
            if name.startswith(SYSTEM_PREFIXES) or name.startswith(("@rpath/", "@executable_path/",
                                                                    "@loader_path/")):
                continue
            raise SystemExit("stage: %s loads %s, which is neither macOS's nor staged" % (path, name))


def stage(out, cache, work, jobs, nice, use_pgo=True, profile=None, keep_build=False, host_notices=True,
          notices_extra=()):
    started = time.time()
    out = out.resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    work.mkdir(parents=True, exist_ok=True)
    log("Python")
    python_runtime.stage(cache, out / "python")
    build_dir = build_llvm(cache, work, jobs, nice, use_pgo, profile, keep_build)
    log("LLVM, staging")
    llvm.stage(build_dir, out / "llvm", work / "probe")
    if not keep_build:
        shutil.rmtree(build_dir)
    log("notices")
    groups = ("toolchain", "host") if host_notices else ("toolchain",)
    notices.write(out / "THIRD_PARTY_NOTICES.txt", cache, out / "python", groups, extra=notices_extra)
    macho_files.verify_signatures(out)
    check_links(out)
    pins = fetch.load_pins()
    files = manifest(out)
    record = {"schema": "recomp-toolchain/1",
              "llvm": {"version": pins["llvm"]["version"], "source": pins["llvm"]["source"],
                       "pgo": bool(use_pgo), "targets": "AArch64",
                       "deployment_target": llvm.DEPLOYMENT_TARGET},
              "python": {k: pins["python"][k] for k in ("version", "url", "sha256")},
              "wheels": [{k: w[k] for k in ("name", "version", "url", "sha256")} for w in pins["wheels"]],
              "invocation": {"python": "python/bin/python3 -I -B", "cc": "llvm/bin/clang",
                             "lld": "llvm/bin/ld64.lld"},
              "files": files}
    (out / "MANIFEST.json").write_text(json.dumps(record, indent=1, sort_keys=True) + "\n")
    size = sum(f.get("size", 0) for f in files.values())
    log("%s: %d files, %.1f MB, in %.0f s" % (out, len(files), size / 1e6, time.time() - started))
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True, help="the toolchain directory to create")
    parser.add_argument("--cache", type=Path, default=DEFAULT_CACHE, help="downloads (kept)")
    parser.add_argument("--work", type=Path, help="LLVM source and build trees (default: <cache>/work)")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 4))
    parser.add_argument("--nice", type=int, default=15)
    parser.add_argument("--profile", type=Path, help="a merged LLVM profile to reuse (skips PGO stage 1)")
    parser.add_argument("--no-pgo", action="store_true")
    parser.add_argument("--keep-build", action="store_true")
    parser.add_argument("--no-host-notices", action="store_true",
                        help="notices for the toolchain only, not the macOS host app")
    parser.add_argument("--notices-extra", type=Path, action="append", default=[],
                        help="a JSON list of the game's own shipped components (notices.load_extra)")
    args = parser.parse_args(argv)
    stage(args.out, args.cache.resolve(), (args.work or args.cache / "work").resolve(), args.jobs, args.nice,
          use_pgo=not args.no_pgo, profile=args.profile.resolve() if args.profile else None,
          keep_build=args.keep_build, host_notices=not args.no_host_notices,
          notices_extra=[p.resolve() for p in args.notices_extra])
    return 0


if __name__ == "__main__":
    sys.exit(main())
