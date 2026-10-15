# The bundled first-run toolchain

An app that translates a game on the player's own Mac needs a Python for the
translator and a C compiler and linker for the bundles (`tools/recomp/bundle.py`),
and a player's Mac has neither: without the Xcode Command Line Tools,
`/usr/bin/python3`, `nm` and `clang` are stubs that offer to install them. These
scripts stage a toolchain the app carries instead. They run on the packager's
Mac; the player only runs the result.

```sh
.venv/bin/python tools/toolchain/stage.py --out build/toolchain \
    [--cache DIR] [--work DIR] [--jobs N] [--nice N] [--profile FILE] [--no-pgo] [--keep-build] \
    [--no-host-notices] [--notices-extra JSON]
```

| Path in `--out` | What | Size [measured] |
|---|---|---|
| `python/bin/python3` | CPython 3.12.13 (python-build-standalone 20260303), trimmed, with capstone 5.0.9 and pefile 2024.8.26 | 44.1 MB, 1,139 files (10.1 MB as `.tar.xz`) |
| `llvm/bin/llvm` | clang and lld 23.1.3, AArch64 only, one multicall binary | 76.1 MB (21.1 MB as `.tar.xz`) |
| `llvm/bin/clang`, `llvm/bin/ld64.lld` | relative symlinks to `llvm` | |
| `llvm/lib/clang/23/include/` | the 10 compiler headers a freestanding compile reads | 41 KB |
| `THIRD_PARTY_NOTICES.txt` | every licence text the app must carry (`notices.py`) | 0.35 MB |
| `MANIFEST.json` | SHA-256, size and mode of every file, symlink targets, the pins | |
| **total** | | **120.6 MB; 31.4 MB as `.tar.xz`** |

## Using it

```sh
T=<the toolchain directory, e.g. EmpireEarth.app/Contents/Resources/toolchain>
"$T/python/bin/python3" -I -B tools/recomp/bundle.py --gen ... --out ... \
    --cc "$T/llvm/bin/clang" --lld "$T/llvm/bin/ld64.lld"      # or RECOMP_FREESTANDING_CC / RECOMP_LD64_LLD
```

- **Python: always `-I -B`.** `-I` ignores `PYTHON*` variables, the user's
  site-packages and the script's directory (the kit's tools put their own
  directories on `sys.path`); `-B` writes no `.pyc`. The standard library and
  the wheels ship precompiled as unchecked-hash `.pyc`, so nothing ever needs
  writing: an app must not write into itself (its signature seals it, and App
  Translocation runs it read-only). `PYTHONDONTWRITEBYTECODE` does not work
  here, because `-I` ignores it; a `site-packages/sitecustomize.py` sets
  `sys.dont_write_bytecode` instead, so a tool that starts the interpreter
  with `-I` alone writes nothing either.
- **clang finds its headers by itself.** The driver dispatches on the name it
  is called by, and clang's resource directory is `bin/../lib/clang/23` from the
  binary's real path, so keep `bin/` and `lib/` together; the tree may be copied
  anywhere. `bundle.py` asks `clang -print-resource-dir` and passes
  `-isystem <it>/include`.
- **No SDK, ever.** The staged clang has no default sysroot and no
  configuration file, and `bundle.py` runs it with `--no-default-config` and
  without `SDKROOT`, `CPATH` and the other variables clang reads
  (`TOOL_ENV_IGNORED`). A full first-run build in a sandbox that hides Xcode,
  the Command Line Tools and Homebrew gives the same bundles.
- Everything is relocatable and read-only safe; `tools/tests/test_toolchain.py`
  with `RECOMP_STAGED_TOOLCHAIN=<dir>` runs a read-only copy under a path with
  spaces and checks that nothing in it changed.

## How it is made

Every input is in `pins.json` with its URL and SHA-256, and a download is used
only if it matches (`fetch.py`). Downloads are cached in `--cache` (default
`~/Library/Caches/recomp-kit/toolchain`).

**LLVM (`llvm.py`, `pgo.py`).** Built from `llvm-project-23.1.3.src.tar.xz`, the
same release Homebrew's llvm and lld 23.1.3 come from. Its sigstore attestation
(`.jsonl`) was checked when it was pinned: `gh attestation verify <file> --bundle
<file>.jsonl --repo llvm/llvm-project` (signed by `release-sources.yml` at the tag).

- `LLVM_ENABLE_PROJECTS=clang;lld`, `LLVM_TARGETS_TO_BUILD=AArch64`, Release, no
  assertions, no plugins, no static analyzer.
- `LLVM_TOOL_LLVM_DRIVER_BUILD=ON` with `LLVM_DISTRIBUTION_COMPONENTS=clang;lld;...`:
  the driver holds clang and lld only (no llvm-ar, dsymutil and the rest).
- No zstd, libxml2, libedit, libpfm, ffi or curl: the binary links only
  `libSystem`, `libc++` and `libz`, and `-dead_strip_dylibs` drops the
  CoreFoundation that dsymutil would want. Deployment target 13.0, bundle.py's
  `--min-macos`.
- ThinLTO, and profile-guided optimisation: an IR-instrumented build compiles a
  corpus with bundle.py's own flags and links it with its ld64.lld, then the
  final build uses the merged profile. The corpus is no game's: `pgo.py` makes
  it from the translator's synthetic instruction vocabulary (the Unicorn suite's
  cases, `tools/recomp/tests/test_translate_insns.py`), assembled by a seeded
  generator into 32 files of about 1 MB whose functions have the size spread of
  a real translation (median about 1.5 KB of C, a tail past 100 KB). The raw
  profiles are merged with the host compiler's `llvm-profdata` (`xcrun`).
- `strip -x`, then `codesign -s - -f`.

**Python (`python_runtime.py`).** The python-build-standalone `install_only`
archive for aarch64-apple-darwin (it is relocatable: the interpreter finds its
prefix from its own path). Removed: Tcl/Tk and tkinter, IDLE, pip and
ensurepip, lib2to3, pydoc_data, turtle, headers, man pages,
`config-3.12-darwin` (the static library), the test extension modules. The
capstone and pefile wheels are unpacked into `site-packages` (no pip; capstone's
C headers dropped). capstone must be exactly the pinned version: a game's
translation inputs are pinned to its decoding (its `__version__` says 5.0.7;
the distribution's metadata, which the self-test checks, says 5.0.9). Then
`strip -x`, ad-hoc signatures, unchecked-hash `.pyc` for everything, and a
self-test that runs `-I -B`, checks the wheel versions and decodes x86.

**Notices (`notices.py`).** Full texts, never summaries, from where each
component's release puts them: the LLVM source archive (llvm, clang, lld and
libc `LICENSE.TXT`, BLAKE3's, and the leading terms of the Support files that
carry their own: regex, strlcpy, ConvertUTF, the Unicode name tables, MD5,
xxhash), the staged CPython and wheels, CPython's `Doc/license.rst`,
python-build-standalone's licence files for what its build links into
libpython (OpenSSL, libffi, bzip2, liblzma, SQLite, mpdecimal, expat, libuuid;
`PYTHON.json` of the same build lists them), capstone's `LICENSE_LLVM.TXT`
from its source archive, and the kit's `third_party/` files and pinned SDL and
glslang licences for the macOS host app. `--no-host-notices` leaves the host's
components out; `--notices-extra <json>` adds a game repository's own shipped
components (`notices.load_extra`: title, licence, source, place, licence files).

## Building it: time, disk and prerequisites

On an M4 Max (16 cores, shared with other work, `--jobs 12 --nice 15`)
[measured]: the instrumented build 7.0 min, the training 1 min, the final ThinLTO
+ PGO build about 10 min, Python and notices under a minute: about 19 min in all. Peak disk about 2.6 GB
(downloads 0.25 GB, source 0.34 GB, one build tree at a time at 1.6-1.9 GB, the
corpus, its objects and the profile 0.17 GB); `stage.py` deletes the build trees once staged unless
`--keep-build`, and `--profile <merged .profdata>` skips the instrumented build
and the training.

Needs: Xcode or the Command Line Tools (the host compiler, its SDK, `strip`,
`otool`, `xcrun llvm-profdata`), CMake and Ninja, the network, and the kit's
development venv (the PGO corpus imports the instruction suite, which needs
`unicorn` and `pytest`).

## What it was measured against

The same nine-module translation (269 MB of C), compiled and linked by the
staged toolchain and by Homebrew's clang and lld 23.1.3:

| | wall, 12 jobs | CPU-s | bundles vs Homebrew's |
|---|---|---|---|
| Homebrew 23.1.3 (PGO + ThinLTO, dylibs) | 108.6 / 114.5 s | 1,253 / 1,304 | - |
| **staged, ThinLTO + PGO** | **108.4 / 110.9 s** | **1,255 / 1,287** | **all 9 byte-identical** |
| staged, ThinLTO only (`--no-pgo`) | 129.6 / 132.3 s | 1,507 / 1,536 | all 9 byte-identical |

Two runs each, alternated, on a machine other jobs were loading. The objects
differ from Homebrew's in two bytes, the SDK version in `LC_BUILD_VERSION`
(Homebrew's clang finds the Command Line Tools' SDK by itself; the staged one
records none); lld takes the bundle's from `-platform_version`, so the bundles
are the same.

The official `LLVM-23.1.3-macOS-ARM64.tar.zst` was the alternative: 871 MB to
download, and its `clang` and `lld` are 172 + 131 MB even stripped, because each
links all of LLVM for every target. The build here is 76 MB for both.

## Reproducibility

- **From a profile, the build is bit-for-bit repeatable** [measured]: two final
  builds from the same profile into different directories gave the same
  `llvm` (SHA-256 `0cf89873…` before stripping), and two stagings into
  different directories the same `MANIFEST.json` (the `.pyc` carry relative file
  names). `MANIFEST.json` records the profile's SHA-256 (`llvm.profile_sha256`).
- **The training is not**: two trainings with the same instrumented compiler
  gave profiles that agree on 99.98% of edge counts, not all (lld's threads,
  hash-table orders). A fresh stage therefore gives an `llvm` with other bytes
  and the same behaviour. What it compiles does not depend on it: the PGO and
  non-PGO builds above give the same bundles.
- So keep the merged profile (`<work>/pgo/llvm.profdata`, 53 MB, 6 MB as
  `.xz`) with a release, and `--profile` it to rebuild that release's binary.

## Changing a pin

Edit `pins.json` (and, for capstone or pefile, `tools/requirements.txt`), check
the new artifact's attestation, restage, and run the game's first-run pipeline
against it: the translation inputs must still hash to the game's pins and the
bundles should not change. A new LLVM also needs `RESOURCE_HEADERS` (checked by
the probe compile in `llvm.stage`) and the version strings in `notices.py`.
