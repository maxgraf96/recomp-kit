#!/usr/bin/env python3
"""Collect the full licence texts of everything an app built with the kit ships.

    tools/toolchain/notices.py --cache DIR --python STAGED/python --out THIRD_PARTY_NOTICES.txt [--no-host]

One plain-text file, for the app's About box and for anyone who asks. Two
groups of components:

  * toolchain - the bundled first-run toolchain (stage.py): LLVM's clang and
    lld with the third-party code inside them, CPython with what
    python-build-standalone links into it, capstone and pefile;
  * host - what the macOS host app (recomp_app) links or carries: SDL3,
    FFmpeg, glslang, volk and the Vulkan headers, Lua, miniz, SoftFloat,
    stb_truetype, TinySoundFont, GothicKit/dmusic, minimp3, Open Sans, the
    GeneralUser GS bank, and the kit itself (NOTICE says which code needs
    which; tools/tests/test_toolchain.py keeps the two in step).

Every text is read from where that component's own release puts it: the
pinned LLVM source archive, the staged Python runtime and wheels, the kit's
third_party/ directory, or a pinned download (URL and SHA-256 below). Nothing
is paraphrased: a component whose terms ask for its notice gets the notice.
A game repository adds what its own app carries with --extra (load_extra).
"""

import argparse
import json
from pathlib import Path
import sys
import tarfile

HERE = Path(__file__).resolve().parent
KIT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import fetch  # noqa: E402

RAW = "https://raw.githubusercontent.com/"


def url(path, sha256):
    return ("url", RAW + path, sha256)


# (kind, ...) text sources:
#   ("llvm", path)            a file of the pinned llvm-project source archive
#   ("llvm-header", path)     the leading comment block of such a file (its own terms)
#   ("python", path)          a file of the staged runtime (relative to python/)
#   ("kit", path)             a file of this kit
#   ("url", url, sha256)      a pinned download
#   ("sdist", pin, member)    a member of a pinned source archive
#   ("file", path)            a file a game repository names (load_extra)
COMPONENTS = [
    # -- the toolchain ------------------------------------------------------
    dict(key="llvm", group="toolchain", title="LLVM 23.1.3: clang, lld and LLVM",
         licence="Apache-2.0 WITH LLVM-exception",
         source="https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.3",
         shipped="llvm/bin/llvm (run as clang and ld64.lld) and ten of clang's headers in "
                 "llvm/lib/clang/23/include",
         note="Built from the source release by tools/toolchain/llvm.py. Code clang compiles is not "
              "covered by these terms (the LLVM exception).",
         texts=[("llvm", "llvm/LICENSE.TXT"), ("llvm", "clang/LICENSE.TXT"), ("llvm", "lld/LICENSE.TXT"),
                ("llvm", "libc/LICENSE.TXT")]),
    dict(key="llvm-third-party", group="toolchain", title="Third-party code inside LLVM's Support library",
         licence="BSD-3-Clause (regex), ISC (strlcpy), Unicode-DFS (ConvertUTF, Unicode names), "
                 "public domain (MD5), BSD-2-Clause (xxhash), CC0-1.0 OR Apache-2.0 (BLAKE3)",
         source="https://github.com/llvm/llvm-project/tree/llvmorg-23.1.3/llvm/lib/Support",
         shipped="inside llvm/bin/llvm",
         texts=[("llvm-header", "llvm/lib/Support/regcomp.c"), ("llvm-header", "llvm/lib/Support/regstrlcpy.c"),
                ("llvm-header", "llvm/lib/Support/ConvertUTF.cpp"),
                ("llvm-header", "llvm/lib/Support/UnicodeNameToCodepointGenerated.cpp"),
                ("llvm-header", "llvm/lib/Support/MD5.cpp"), ("llvm-header", "llvm/lib/Support/xxhash.cpp"),
                ("llvm", "llvm/lib/Support/BLAKE3/LICENSE")]),
    dict(key="cpython", group="toolchain", title="CPython 3.12.13", licence="PSF-2.0",
         source="https://www.python.org/downloads/release/python-31213/",
         shipped="python/ (the interpreter, libpython3.12.dylib and the standard library)",
         texts=[("python", "lib/python3.12/LICENSE.txt"),
                url("python/cpython/v3.12.13/Doc/license.rst",
                    "341832873fd316a37927e79385093fbbfd40a467428480835fe435a80cadf4e5")]),
    dict(key="python-build-standalone", group="toolchain",
         title="python-build-standalone 20260303 and the libraries it links into CPython",
         licence="MPL-2.0 (build system); OpenSSL 3: Apache-2.0; libffi: MIT; bzip2: bzip2-1.0.6; "
                 "liblzma: 0BSD; SQLite: public domain; mpdecimal: BSD-2-Clause; expat: MIT; "
                 "libuuid: BSD-3-Clause",
         source="https://github.com/astral-sh/python-build-standalone/releases/tag/20260303",
         shipped="statically inside python/lib/libpython3.12.dylib",
         note="The aarch64-apple-darwin build's PYTHON.json lists these as its non-system links; "
              "Tcl/Tk is removed with tkinter, and ncurses, libedit and zlib are macOS's own.",
         texts=[url("astral-sh/python-build-standalone/20260303/LICENSE",
                    "1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5")] +
               [url("astral-sh/python-build-standalone/20260303/LICENSE.%s.txt" % name, digest) for name, digest in (
                   ("openssl-3", "7d5450cb2d142651b8afa315b5f238efc805dad827d91ba367d8516bc9d49e7a"),
                   ("libffi", "deaf3a42effb551a5b140fa9afefed183a27f1341c6d1bf430d106a5e6931fc0"),
                   ("bzip2", "1f38bbc7caacafd65169276d759c0d88c991b753b643ce35d0e45ea1971dd441"),
                   ("liblzma", "9a4062de0a2c388a98cf35a35d348b62fa97c838a71c3c28ee1a2d7d0a565b02"),
                   ("sqlite", "38bef3d28b24f145ea293bd3b6eb4b20396982abc8303128fb493986ea5bc719"),
                   ("mpdecimal", "669512af7219f58be03a398766d7c9da11a3b3df9d3f05cb74c5ceca25c8da3b"),
                   ("expat", "122f2c27000472a201d337b9b31f7eb2b52d091b02857061a8880371612d9534"),
                   ("libuuid", "122ee1f7e258f2c3c0e538a75c037684f420454bf3850ddc74ce750bbf5fe86b"))]),
    dict(key="capstone", group="toolchain", title="Capstone 5.0.9", licence="BSD-3-Clause; tables derived "
         "from LLVM under the LLVM Release License (NCSA)",
         source="https://pypi.org/project/capstone/5.0.9/",
         shipped="python/lib/python3.12/site-packages/capstone",
         texts=[("python", "lib/python3.12/site-packages/capstone-5.0.9.dist-info/LICENSE.TXT"),
                ("sdist", "capstone", "capstone-5.0.9/src/LICENSE_LLVM.TXT")]),
    dict(key="pefile", group="toolchain", title="pefile 2024.8.26", licence="MIT",
         source="https://pypi.org/project/pefile/2024.8.26/",
         shipped="python/lib/python3.12/site-packages/pefile.py, peutils.py, ordlookup",
         texts=[("python", "lib/python3.12/site-packages/pefile-2024.8.26.dist-info/LICENSE")]),
    # -- the macOS host app -------------------------------------------------
    dict(key="recomp-kit", group="host", title="recomp-kit", licence="MIT",
         source="the kit's repository (its LICENSE and NOTICE follow)", shipped="the host app's native code",
         texts=[("kit", "LICENSE"), ("kit", "NOTICE")]),
    dict(key="sdl3", group="host", title="SDL 3.4.16", licence="Zlib", source="https://github.com/libsdl-org/SDL",
         shipped="statically linked into the host", version_pin=("SDL3", "release-3.4.16"),
         texts=[url("libsdl-org/SDL/release-3.4.16/LICENSE.txt",
                    "1c040b8271b37e5076359f8fd54240e371114112924d2df81ef87c7d6a1dfdfd")]),
    dict(key="ffmpeg", group="host", title="FFmpeg 7.1.1", licence="LGPL-2.1-or-later",
         source="https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz",
         shipped="Contents/Frameworks/libavformat, libavcodec, libavutil (dynamically linked, replaceable)",
         note="Unmodified; the exact source, its SHA-256 and the configure command are below. The LGPL "
              "lets you replace these libraries with your own build: re-sign the app ad hoc afterwards "
              "(codesign --force --deep -s -).",
         texts=[("kit", "third_party/ffmpeg/NOTICE.md")]),
    dict(key="glslang", group="host", title="glslang 16.6.0",
         licence="BSD-3-Clause, BSD-2-Clause, MIT, Apache-2.0 (the GPL-3.0 text below covers a Bison "
                 "grammar glslang no longer uses)",
         source="https://github.com/KhronosGroup/glslang", shipped="statically linked into the host",
         version_pin=("glslang", "16.6.0"),
         texts=[url("KhronosGroup/glslang/16.6.0/LICENSE.txt",
                    "17e70c676e1521ff3e4686f04a2053d93a7e28a33be8de7ec37ab0ff72feb677")]),
    dict(key="volk", group="host", title="volk", licence="MIT", source="https://github.com/zeux/volk",
         shipped="statically linked into the host", texts=[("kit", "third_party/volk/LICENSE.md")]),
    dict(key="vulkan-headers", group="host", title="Vulkan-Headers", licence="Apache-2.0 OR MIT",
         source="https://github.com/KhronosGroup/Vulkan-Headers", shipped="compiled into the host",
         texts=[("kit", "third_party/vulkan-headers/LICENSE.md")]),
    dict(key="lua", group="host", title="Lua 5.4.6", licence="MIT", source="https://www.lua.org",
         shipped="statically linked into the host", texts=[("kit", "third_party/lua/LICENSE")]),
    dict(key="miniz", group="host", title="miniz 3.0.2", licence="MIT", source="https://github.com/richgel999/miniz",
         shipped="statically linked into the host", texts=[("kit", "third_party/miniz/LICENSE")]),
    dict(key="softfloat", group="host", title="Berkeley SoftFloat Release 3e", licence="BSD-3-Clause",
         source="http://www.jhauser.us/arithmetic/SoftFloat.html", shipped="statically linked into the host",
         texts=[("kit", "third_party/softfloat/COPYING.txt")]),
    dict(key="stb", group="host", title="stb_truetype 1.26", licence="MIT OR Unlicense",
         source="https://github.com/nothings/stb", shipped="compiled into the host",
         texts=[("kit", "third_party/stb/LICENSE")]),
    dict(key="tsf", group="host", title="TinySoundFont", licence="MIT",
         source="https://github.com/schellingb/TinySoundFont", shipped="compiled into the host",
         texts=[("kit", "third_party/tsf/LICENSE")]),
    dict(key="dmusic", group="host", title="GothicKit dmusic (b73b270b) with its TinySoundFont",
         licence="MIT", source="https://github.com/GothicKit/dmusic", shipped="compiled into the host",
         texts=[("kit", "third_party/dmusic/LICENSE.md")]),
    dict(key="minimp3", group="host", title="minimp3", licence="CC0-1.0", source="https://github.com/lieff/minimp3",
         shipped="compiled into the host", texts=[("kit", "third_party/minimp3/LICENSE")]),
    dict(key="opensans", group="host", title="Open Sans 1.101", licence="Apache-2.0",
         source="https://github.com/googlefonts/opensans", shipped="embedded in the host",
         texts=[("kit", "third_party/fonts/opensans/LICENSE")]),
    dict(key="generaluser-gs", group="host", title="GeneralUser GS 2.0.3", licence="GeneralUser GS License v2.0",
         source="https://www.schristiancollins.com/generaluser.php",
         shipped="Contents/Resources/general-midi.sf2", texts=[("kit", "third_party/soundfonts/generaluser-gs/LICENSE")]),
]

# Source archives a notice is read from (not shipped): pinned like everything else.
SDISTS = {
    "capstone": {"url": "https://files.pythonhosted.org/packages/39/01/6516910f546fbb996068207b9dd0229b14bc8dae223114d5e0e27d3cad11/capstone-5.0.9.tar.gz",
                 "sha256": "0429af292ddc604d3c9344696807e281d5e728db029f00e6a4ea9e3bff1aac9e"},
}


def leading_comments(text):
    """The comment block(s) a source file opens with: where a file states its own terms."""
    lines, in_block = [], False
    for line in text.splitlines():
        stripped = line.strip()
        if in_block or stripped.startswith(("/*", "//")) or not stripped:
            lines.append(line)
            if stripped.startswith("/*") or in_block:
                in_block = "*/" not in stripped[2 if stripped.startswith("/*") else 0:]
            continue
        break
    return "\n".join(lines).strip("\n") + "\n"


class Sources(object):
    """Resolves text sources; the LLVM archive is read in one pass for all its members."""

    def __init__(self, cache, python_dir):
        self.cache = Path(cache)
        self.python_dir = Path(python_dir) if python_dir else None
        self._llvm = None

    def llvm_members(self, components):
        wanted = {src[1] for c in components for src in c["texts"] if src[0] in ("llvm", "llvm-header")}
        if not wanted:
            return {}
        tarball = fetch.fetch(fetch.load_pins()["llvm"]["source"], self.cache)
        found = {}
        with tarfile.open(tarball) as archive:
            for member in archive:
                rel = member.name.split("/", 1)[1] if "/" in member.name else ""
                if rel in wanted:
                    found[rel] = archive.extractfile(member).read().decode("utf-8")
        missing = wanted - set(found)
        if missing:
            raise SystemExit("notices: the LLVM source has no %s" % ", ".join(sorted(missing)))
        return found

    def text(self, source):
        kind = source[0]
        if kind == "llvm":
            return self._llvm[source[1]]
        if kind == "llvm-header":
            return leading_comments(self._llvm[source[1]])
        if kind == "python":
            if self.python_dir is None:
                raise SystemExit("notices: --python is needed for %s" % source[1])
            return (self.python_dir / source[1]).read_text(encoding="utf-8")
        if kind == "kit":
            return (KIT / source[1]).read_text(encoding="utf-8")
        if kind == "url":
            pin = {"url": source[1], "sha256": source[2],
                   "file": "notice-" + source[1][len(RAW):].replace("/", "_")}
            return fetch.fetch(pin, self.cache).read_text(encoding="utf-8")
        if kind == "file":
            return Path(source[1]).read_text(encoding="utf-8")
        if kind == "sdist":
            archive_path = fetch.fetch(SDISTS[source[1]], self.cache)
            with tarfile.open(archive_path) as archive:
                return archive.extractfile(source[2]).read().decode("utf-8")
        raise ValueError("unknown notice source %r" % (source,))


def selected(groups):
    return [c for c in COMPONENTS if c["group"] in groups]


def load_extra(path):
    """A game repository's own shipped components, from a JSON list like COMPONENTS
    ({"title", "licence", "source", "shipped", "note"?, "texts": [file, ...]});
    each text is a path relative to the JSON file."""
    path = Path(path)
    extra = []
    for c in json.loads(path.read_text()):
        missing = {"title", "licence", "source", "shipped", "texts"} - set(c)
        if missing:
            raise SystemExit("notices: %s: a component lacks %s" % (path, ", ".join(sorted(missing))))
        texts = [("file", str((path.parent / t).resolve())) for t in c["texts"]]
        for kind, file in texts:
            if not Path(file).is_file():
                raise SystemExit("notices: %s names %s, which does not exist" % (path, file))
        extra.append(dict(c, key=c.get("key", c["title"]), group="extra", texts=texts))
    return extra


def render(components, sources, title):
    """The notices file: an index, then each component's header and full texts."""
    sources._llvm = sources.llvm_members(components)
    rule, thin = "=" * 78, "-" * 78
    out = [title, "", "This app contains the following third-party software. Each is listed with its",
           "licence, where its source is, and where it is in the app, followed by the",
           "complete licence and notice texts as its authors distribute them.", ""]
    for i, c in enumerate(components, 1):
        out.append("%3d. %s  [%s]" % (i, c["title"], c["licence"]))
    out.append("")
    for i, c in enumerate(components, 1):
        out += [rule, "%d. %s" % (i, c["title"]), "Licence: %s" % c["licence"], "Source: %s" % c["source"],
                "In this app: %s" % c["shipped"]]
        if c.get("note"):
            out.append("Note: %s" % c["note"])
        out.append(rule)
        for source in c["texts"]:
            label = source[1] if source[0] != "url" else source[1][len(RAW):]
            if source[0] == "sdist":
                label = source[2]
            if source[0] == "file":
                label = Path(source[1]).name           # not this machine's path
            out += ["", thin, label, thin, sources.text(source).rstrip("\n")]
        out.append("")
    return "\n".join(out) + "\n"


def write(out, cache, python_dir, groups=("toolchain", "host"), title="THIRD-PARTY NOTICES", extra=()):
    components = selected(groups) + [c for path in extra for c in load_extra(path)]
    text = render(components, Sources(cache, python_dir), title)
    Path(out).write_text(text, encoding="utf-8")
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache", type=Path, required=True, help="downloads")
    parser.add_argument("--python", type=Path, help="the staged python/ directory")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--no-host", action="store_true", help="only the toolchain's components")
    parser.add_argument("--extra", type=Path, action="append", default=[],
                        help="a JSON list of further components the app ships (load_extra)")
    args = parser.parse_args(argv)
    groups = ("toolchain",) if args.no_host else ("toolchain", "host")
    write(args.out, args.cache, args.python, groups, extra=args.extra)
    print("notices: %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
