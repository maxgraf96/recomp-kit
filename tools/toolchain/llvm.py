#!/usr/bin/env python3
"""Build the bundled LLVM toolchain from pinned source: one AArch64-only binary.

    tools/toolchain/llvm.py --cache DIR --work DIR --out DIR [--jobs N] [--no-lto]

The first run on a player's Mac compiles generated C with clang and links it
with ld64.lld (tools/recomp/bundle.py). This builds exactly those two from the
pinned llvm-project source (pins.json) as LLVM's multicall driver: a single
`llvm` executable that is clang when called as `clang` and ld64.lld when
called as `ld64.lld`, so the two share one copy of LLVM.

  * Only the AArch64 backend, clang and lld; no assertions, no plugins, no
    static analyzer, no tests.
  * No optional dependencies: no zstd, libxml2, libedit, libpfm, ffi or curl.
    The result links only libSystem, libc++ and libz, which every macOS has.
  * Deployment target 13.0, bundle.py's default --min-macos.
  * ThinLTO across the whole binary (--no-lto to compare).

The host compiler is the build machine's (Xcode or the Command Line Tools):
building the toolchain is a packaging step, not a player's. The build tree
lives under --work and may be deleted once --out is staged.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(1, str(HERE.parent / "recomp"))
import bundle  # noqa: E402
import fetch  # noqa: E402
import macho_files  # noqa: E402

# bundle.py's --min-macos default: the toolchain runs wherever the bundles do.
DEPLOYMENT_TARGET = "13.0"
# The names the multicall binary is called by.
TOOLS = ("clang", "ld64.lld")
# The compiler's own headers a freestanding compile of generated code reads
# (stage() checks this against a probe's depfile): stddef.h with its pieces,
# stdint.h and limits.h. Nothing else of lib/clang/<major>/include ships.
RESOURCE_HEADERS = ("stddef.h", "__stddef_header_macro.h", "__stddef_max_align_t.h", "__stddef_null.h",
                    "__stddef_offsetof.h", "__stddef_ptrdiff_t.h", "__stddef_size_t.h",
                    "__stddef_wchar_t.h", "stdint.h", "limits.h")

# Only these become part of the multicall binary (AddLLVM.cmake builds every
# GENERATE_DRIVER tool in LLVM_DISTRIBUTION_COMPONENTS into llvm-driver).
COMPONENTS = ("clang", "lld", "llvm-driver", "clang-resource-headers")

# The subtrees of llvm-project the build reads; tests and docs are left out.
# LLVM's own build reads a few of LLVM libc's headers (the shared math
# utilities, FindLibcCommonUtils.cmake) and lld's Mach-O port libunwind's.
SOURCE_DIRS = ("llvm", "clang", "lld", "cmake", "third-party", "libc", "libunwind/include")
SOURCE_SKIP = ("llvm/test/", "clang/test/", "lld/test/", "libc/test/", "llvm/unittests/",
               "clang/unittests/", "lld/unittests/", "llvm/docs/", "clang/docs/", "libc/docs/",
               "libc/benchmarks/", "libc/fuzzing/", "libc/examples/")


def cmake_args(source, build, lto=True, profile=None, instrument=False):
    """The CMake configuration of the bundled toolchain."""
    args = [
        "-G", "Ninja", "-S", str(source / "llvm"), "-B", str(build),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_OSX_ARCHITECTURES=arm64",
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=%s" % DEPLOYMENT_TARGET,
        "-DLLVM_ENABLE_PROJECTS=clang;lld",
        "-DLLVM_TARGETS_TO_BUILD=AArch64",
        "-DLLVM_TOOL_LLVM_DRIVER_BUILD=ON",
        "-DLLVM_DISTRIBUTION_COMPONENTS=%s" % ";".join(COMPONENTS),
        "-DLLVM_ENABLE_ASSERTIONS=OFF",
        "-DLLVM_ENABLE_ZSTD=OFF", "-DLLVM_ENABLE_LIBXML2=OFF", "-DLLVM_ENABLE_ZLIB=ON",
        "-DLLVM_ENABLE_LIBEDIT=OFF", "-DLLVM_ENABLE_LIBPFM=OFF", "-DLLVM_ENABLE_FFI=OFF",
        "-DLLVM_ENABLE_CURL=OFF", "-DLLVM_ENABLE_HTTPLIB=OFF", "-DLLVM_ENABLE_Z3_SOLVER=OFF",
        "-DLLVM_ENABLE_PLUGINS=OFF", "-DCLANG_PLUGIN_SUPPORT=OFF",
        "-DCLANG_ENABLE_STATIC_ANALYZER=OFF", "-DCLANG_ENABLE_OBJC_REWRITER=OFF",
        "-DLLVM_ENABLE_BINDINGS=OFF",
        "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_EXAMPLES=OFF", "-DLLVM_INCLUDE_BENCHMARKS=OFF",
        "-DLLVM_INCLUDE_DOCS=OFF", "-DLLVM_INCLUDE_UTILS=OFF", "-DCLANG_INCLUDE_TESTS=OFF",
        "-DCLANG_INCLUDE_DOCS=OFF",
        # Frameworks the driver names but clang and lld never call (dsymutil's
        # CoreFoundation) are not loaded.
        "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-dead_strip_dylibs",
        "-DLLVM_PARALLEL_LINK_JOBS=1",
    ]
    if lto and not instrument:
        args.append("-DLLVM_ENABLE_LTO=Thin")
    if instrument:
        # Enough value-profile counters for LLVM's indirect calls (as
        # Homebrew's PGO build does), or the runtime drops some and warns.
        counters = "-Xclang -mllvm -Xclang -vp-counters-per-site=6"
        args += ["-DLLVM_BUILD_INSTRUMENTED=IR",
                 "-DCMAKE_C_FLAGS=%s" % counters, "-DCMAKE_CXX_FLAGS=%s" % counters]
    if profile:
        args += ["-DLLVM_PROFDATA_FILE=%s" % profile,
                 # A profile from a smaller build names functions this one lacks.
                 "-DCMAKE_C_FLAGS=-Wno-backend-plugin", "-DCMAKE_CXX_FLAGS=-Wno-backend-plugin"]
    return args


def unpack_source(tarball, work):
    """The llvm-project tree the build needs, unpacked once under `work`."""
    with tarfile.open(tarball) as archive:
        top = archive.next().name.split("/", 1)[0]
        source = Path(work) / top
        if (source / ".unpacked").is_file():
            return source
        members = []
        for member in archive:
            rel = member.name.split("/", 1)[1] if "/" in member.name else ""
            if rel == "LICENSE.TXT" or (rel.startswith(tuple(d + "/" for d in SOURCE_DIRS))
                                        and not rel.startswith(SOURCE_SKIP)):
                members.append(member)
        archive.extractall(work, members=members, filter="tar")
    (source / ".unpacked").write_text("")
    return source


def nice_prefix(level):
    return ["nice", "-n", str(level)] if level else []


def build(source, build_dir, jobs, nice=15, lto=True, profile=None, instrument=False):
    """Configure and build the driver; return the path of the `llvm` binary."""
    if not (build_dir / "build.ninja").is_file():
        subprocess.run(["cmake"] + cmake_args(source, build_dir, lto, profile, instrument), check=True)
    subprocess.run(nice_prefix(nice) + ["ninja", "-C", str(build_dir), "-j", str(jobs),
                                        "llvm-driver", "clang-resource-headers"], check=True)
    return build_dir / "bin" / "llvm"


def probe_resource_headers(clang, scratch):
    """The compiler headers a freestanding compile of the runtime's headers reads.

    Compiles a file that includes every header generated code includes, with
    bundle.py's own flags, and returns the resource-directory files its
    depfile names (relative to <resource>/include)."""
    resource = Path(subprocess.check_output([str(clang), "-print-resource-dir"], text=True).strip())
    scratch.mkdir(parents=True, exist_ok=True)
    probe, obj, dep = scratch / "probe.c", scratch / "probe.o", scratch / "probe.d"
    probe.write_text('#include "x86.h"\n#include "intrinsics.h"\n#include "seh.h"\n'
                     "int recomp_probe(void) { return 0; }\n")
    defines = ["GUEST_IMAGE_BASE=0x00400000u", "GUEST_HEAP_BASE=0x01000000u", "GUEST_SIZE=0x10000000u"]
    flags = bundle.compile_flags(str(clang), scratch, scratch, defines, DEPLOYMENT_TARGET)
    subprocess.run([str(clang)] + flags + ["-MD", "-MF", str(dep), "-c", str(probe), "-o", str(obj)], check=True,
                   env=bundle.tool_env())
    include = (resource / "include").resolve()
    used = set()
    for word in dep.read_text().replace("\\\n", " ").split(":", 1)[1].split():
        path = Path(word).resolve()
        if include in path.parents:
            used.add(path.relative_to(include).as_posix())
    return sorted(used)


def stage(build_dir, out, scratch, strip=True, sign=True):
    """Stage <out>/bin/{llvm, clang, ld64.lld} and the compiler headers bundle.py needs.

    clang and ld64.lld are relative symlinks to the one binary: the driver
    dispatches on the name it was called by, and clang finds its resource
    directory (bin/../lib/clang/<major>) from the binary's real path, so
    the tree works from wherever it is copied."""
    if out.exists():
        shutil.rmtree(out)
    bin_dir = out / "bin"
    bin_dir.mkdir(parents=True)
    binary = bin_dir / "llvm"
    shutil.copy2(build_dir / "bin" / "llvm", binary)
    if strip:
        macho_files.strip_local_symbols(out, [binary])
    if sign:
        macho_files.sign_ad_hoc(out, [binary])
    for name in TOOLS:
        (bin_dir / name).symlink_to("llvm")
    major = fetch.load_pins()["llvm"]["version"].split(".")[0]
    source_include = build_dir / "lib" / "clang" / major / "include"
    include = out / "lib" / "clang" / major / "include"
    include.mkdir(parents=True)
    for name in RESOURCE_HEADERS:
        shutil.copy2(source_include / name, include / name)
    # Every header the freestanding compile reads is one of those shipped.
    used = probe_resource_headers(bin_dir / "clang", scratch)
    missing = sorted(set(used) - set(RESOURCE_HEADERS))
    if missing:
        raise SystemExit("llvm: a freestanding compile needs %s, which RESOURCE_HEADERS leaves out"
                         % ", ".join(missing))
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache", type=Path, required=True, help="downloads")
    parser.add_argument("--work", type=Path, required=True, help="source and build trees")
    parser.add_argument("--out", type=Path, help="stage the toolchain here (the llvm/ directory)")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 4))
    parser.add_argument("--nice", type=int, default=15)
    parser.add_argument("--no-lto", action="store_true")
    parser.add_argument("--instrument", action="store_true", help="an IR-instrumented build (PGO stage 1)")
    parser.add_argument("--profile", type=Path, help="an indexed profile to optimise with (PGO stage 2)")
    parser.add_argument("--build-name", default="llvm-build")
    args = parser.parse_args(argv)
    pins = fetch.load_pins()
    tarball = fetch.fetch(pins["llvm"]["source"], args.cache)
    args.work.mkdir(parents=True, exist_ok=True)
    source = unpack_source(tarball, args.work)
    build_dir = args.work / args.build_name
    binary = build(source, build_dir, args.jobs, args.nice, lto=not args.no_lto, profile=args.profile,
                   instrument=args.instrument)
    print("llvm: %s" % binary)
    if args.out:
        stage(build_dir, args.out.resolve(), args.work / "probe")
        print("llvm: staged %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
