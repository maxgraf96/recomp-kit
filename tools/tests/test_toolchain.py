"""The bundled first-run toolchain (tools/toolchain/): pins, fetching, the Python
trim, the LLVM configuration, the PGO corpus, notices and the manifest.

    .venv/bin/python -m pytest -q tools/tests/test_toolchain.py

Nothing here downloads or builds LLVM. With RECOMP_STAGED_TOOLCHAIN=<dir> (a
tools/toolchain/stage.py output) the staged tree itself is checked too: it
runs from a read-only copy at a path with a space, writes nothing, and builds
a freestanding bundle with no other toolchain.
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys
import zipfile

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "toolchain"))
sys.path.insert(0, str(ROOT / "tools" / "recomp"))
import bundle  # noqa: E402
import fetch  # noqa: E402
import llvm  # noqa: E402
import macho_files  # noqa: E402
import notices  # noqa: E402
import python_runtime  # noqa: E402
import stage  # noqa: E402

SHA = re.compile(r"\A[0-9a-f]{64}\Z")


def test_every_pin_is_an_https_url_with_a_sha256():
    pins = fetch.load_pins()
    entries = [pins["llvm"]["source"], pins["python"]] + pins["wheels"] + list(notices.SDISTS.values())
    for pin in entries:
        assert pin["url"].startswith("https://") and SHA.match(pin["sha256"]), pin
    for component in notices.COMPONENTS:
        for source in component["texts"]:
            if source[0] == "url":
                assert source[1].startswith("https://") and SHA.match(source[2]), source


def test_the_wheels_are_the_kits_own_pins():
    """capstone must be exactly the version the translator's inputs were pinned with."""
    required = dict(line.split("==") for line in (ROOT / "tools/requirements.txt").read_text().split()
                    if "==" in line)
    for wheel in fetch.load_pins()["wheels"]:
        assert required[wheel["name"]] == wheel["version"]
        assert "-%s-" % wheel["version"] in wheel["url"]


def test_fetch_verifies_reuses_and_refuses(tmp_path):
    source = tmp_path / "upstream.bin"
    source.write_bytes(b"pinned bytes")
    digest = hashlib.sha256(b"pinned bytes").hexdigest()
    cache = tmp_path / "cache"
    got = fetch.fetch({"url": source.as_uri(), "sha256": digest}, cache)
    assert got.read_bytes() == b"pinned bytes" and got.parent == cache
    source.unlink()                                         # cached: no download
    assert fetch.fetch({"url": source.as_uri(), "sha256": digest}, cache) == got
    with pytest.raises(SystemExit, match="not the pinned"):
        fetch.fetch({"url": source.as_uri(), "sha256": "0" * 64}, cache)
    other = tmp_path / "other.bin"
    other.write_bytes(b"tampered")
    with pytest.raises(SystemExit, match="has SHA-256"):
        fetch.fetch({"url": other.as_uri(), "sha256": digest}, tmp_path / "cache2")
    assert list((tmp_path / "cache2").iterdir()) == []      # nothing half-written is left


def test_the_llvm_configuration():
    args = llvm.cmake_args(Path("/src"), Path("/build"))
    assert "-DLLVM_TARGETS_TO_BUILD=AArch64" in args and "-DLLVM_TOOL_LLVM_DRIVER_BUILD=ON" in args
    assert "-DLLVM_ENABLE_PROJECTS=clang;lld" in args and "-DLLVM_ENABLE_ASSERTIONS=OFF" in args
    for dependency in ("ZSTD", "LIBXML2", "LIBEDIT", "LIBPFM", "FFI", "CURL", "HTTPLIB"):
        assert "-DLLVM_ENABLE_%s=OFF" % dependency in args
    default = re.search(r'"--min-macos", default="([0-9.]+)"', (ROOT / "tools/recomp/bundle.py").read_text())
    assert default and llvm.DEPLOYMENT_TARGET == default.group(1)
    assert "-DCMAKE_OSX_DEPLOYMENT_TARGET=%s" % llvm.DEPLOYMENT_TARGET in args
    assert "-DLLVM_ENABLE_LTO=Thin" in args
    components = next(a for a in args if a.startswith("-DLLVM_DISTRIBUTION_COMPONENTS="))
    assert components.split("=", 1)[1].split(";")[:2] == ["clang", "lld"]
    instrumented = llvm.cmake_args(Path("/src"), Path("/build"), instrument=True)
    assert "-DLLVM_BUILD_INSTRUMENTED=IR" in instrumented and "-DLLVM_ENABLE_LTO=Thin" not in instrumented
    profiled = llvm.cmake_args(Path("/src"), Path("/build"), profile="/p.profdata")
    assert "-DLLVM_PROFDATA_FILE=/p.profdata" in profiled and "-DLLVM_ENABLE_LTO=Thin" in profiled
    assert set(llvm.TOOLS) == {"clang", "ld64.lld"}
    assert len(llvm.RESOURCE_HEADERS) == 10 and "stddef.h" in llvm.RESOURCE_HEADERS


def test_the_python_trim_removes_what_the_translator_never_runs(tmp_path):
    root = tmp_path / "python"
    keep = ["bin/python3.12", "lib/libpython3.12.dylib", "lib/python3.12/os.py",
            "lib/python3.12/LICENSE.txt", "lib/python3.12/lib-dynload/_dbm.cpython-312-darwin.so",
            "lib/python3.12/site-packages/README.txt", "lib/python3.12/unittest/__init__.py"]
    drop = ["bin/pip3.12", "bin/idle3.12", "bin/pydoc3.12", "bin/2to3-3.12", "bin/python3.12-config",
            "include/python3.12/Python.h", "share/man/man1/python3.1", "lib/libtcl9.0.dylib",
            "lib/tk9.0/tk.tcl", "lib/python3.12/config-3.12-darwin/libpython3.12.a",
            "lib/python3.12/idlelib/idle.py", "lib/python3.12/tkinter/__init__.py",
            "lib/python3.12/ensurepip/__init__.py", "lib/python3.12/site-packages/pip/__init__.py",
            "lib/python3.12/site-packages/pip-26.0.1.dist-info/RECORD",
            "lib/python3.12/lib-dynload/_tkinter.cpython-312-darwin.so",
            "lib/python3.12/lib-dynload/_ctypes_test.cpython-312-darwin.so",
            "lib/python3.12/__pycache__/os.cpython-312.pyc", "lib/python3.12/turtle.py"]
    for name in keep + drop:
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_text(name)
    (root / "bin/python3").symlink_to("python3.12")
    python_runtime.remove(root, python_runtime.TRIM, "3.12.13")
    assert all((root / name).is_file() for name in keep)
    assert not any((root / name).exists() for name in drop)
    assert (root / "bin/python3").is_symlink()


def test_a_wheel_installs_like_an_installer_would(tmp_path):
    wheel = tmp_path / "demo-1.0-py3-none-any.whl"
    with zipfile.ZipFile(wheel, "w") as archive:
        archive.writestr("demo/__init__.py", "VALUE = 1\n")
        info = zipfile.ZipInfo("demo/lib/libdemo.dylib")
        info.external_attr = 0o100755 << 16
        archive.writestr(info, b"\xcf\xfa\xed\xfe")
        archive.writestr("demo-1.0.dist-info/METADATA", "Name: demo\nVersion: 1.0\n")
    site = tmp_path / "site-packages"
    python_runtime.install_wheel(wheel, site)
    assert (site / "demo/__init__.py").read_text() == "VALUE = 1\n"
    assert os.access(site / "demo/lib/libdemo.dylib", os.X_OK)
    assert not os.access(site / "demo/__init__.py", os.X_OK)
    assert (site / "demo-1.0.dist-info/INSTALLER").read_text() == "recomp-kit\n"
    scripted = tmp_path / "scripted-1.0-py3-none-any.whl"
    with zipfile.ZipFile(scripted, "w") as archive:
        archive.writestr("scripted-1.0.data/scripts/tool", "#!python\n")
    with pytest.raises(SystemExit, match=".data directory"):
        python_runtime.install_wheel(scripted, site)


def test_mach_o_files_are_told_apart_from_lookalikes(tmp_path):
    samples = {"thin": b"\xcf\xfa\xed\xfe" + b"\0" * 28, "fat": b"\xca\xfe\xba\xbe\0\0\0\x02" + b"\0" * 24,
               "java": b"\xca\xfe\xba\xbe\0\0\0\x34" + b"\0" * 24, "text": b"#!/bin/sh\n" * 4, "tiny": b"\xcf"}
    for name, data in samples.items():
        (tmp_path / name).write_bytes(data)
    (tmp_path / "link").symlink_to("thin")
    assert [p.name for p in macho_files.macho_files(tmp_path)] == ["fat", "thin"]


def test_leading_comments_are_a_files_own_terms():
    text = "/*\n * Copyright (c) 1998 Someone\n *\n * Permission granted.\n */\n\n#include <x.h>\nint a; /* no */\n"
    assert notices.leading_comments(text) == "/*\n * Copyright (c) 1998 Someone\n *\n * Permission granted.\n */\n"
    mixed = "// header\n//\n\n/*\nLICENSE\n*/\nnamespace x {}\n"
    assert notices.leading_comments(mixed) == "// header\n//\n\n/*\nLICENSE\n*/\n"


def test_every_host_component_is_in_the_kit_and_its_notice():
    """The host group names what NOTICE describes; fetched code is pinned at the tag CMake fetches."""
    notice = (ROOT / "NOTICE").read_text()
    dependencies = (ROOT / "cmake/Dependencies.cmake").read_text()
    for component in notices.selected(("host",)):
        for source in component["texts"]:
            if source[0] == "kit":
                assert (ROOT / source[1]).is_file(), source
        if component["key"] != "recomp-kit":
            word = component["title"].split()[0 if component["key"] != "dmusic" else 1]
            assert word.lower() in notice.lower(), "NOTICE does not mention %s" % component["title"]
        if "version_pin" in component:
            name, tag = component["version_pin"]
            declared = re.search(r"FetchContent_Declare\(%s\s.*?GIT_TAG (\S+)" % name, dependencies, re.S)
            assert declared and declared.group(1) == tag, (name, tag)
            assert tag in component["texts"][0][1]
    ffmpeg = (ROOT / "third_party/ffmpeg/NOTICE.md").read_text()
    assert "7.1.1" in ffmpeg and "GNU LESSER GENERAL PUBLIC LICENSE" in ffmpeg.upper()


def test_the_notices_file_carries_every_text(tmp_path):
    class Offline(notices.Sources):
        def llvm_members(self, components):
            return {s[1]: "/* terms of %s */\nint x;\n" % s[1] for c in components for s in c["texts"]
                    if s[0] in ("llvm", "llvm-header")}

        def text(self, source):
            if source[0] in ("llvm", "llvm-header", "kit"):
                return super().text(source)
            return "full text of %s\n" % (source[1],)

    components = notices.selected(("toolchain", "host"))
    text = notices.render(components, Offline(tmp_path, None), "NOTICES")
    for i, component in enumerate(components, 1):
        assert "\n%d. %s\n" % (i, component["title"]) in text
    assert "/* terms of llvm/lib/Support/regcomp.c */" in text
    # A file's own terms are its leading comment, not its code; a licence file is whole.
    whole = sum(1 for c in components for s in c["texts"] if s[0] == "llvm")
    assert text.count("int x;") == whole
    assert (ROOT / "third_party/lua/LICENSE").read_text().strip() in text


def test_the_pgo_corpus_is_deterministic_and_translator_shaped(tmp_path):
    pytest.importorskip("unicorn")                     # the synthetic cases' module imports it
    import pgo
    first = pgo.corpus(tmp_path / "a", chunks=2, chunk_bytes=64 << 10)
    second = pgo.corpus(tmp_path / "b", chunks=2, chunk_bytes=64 << 10)
    assert [p.read_bytes() for p in first] == [p.read_bytes() for p in second]
    chunk = first[0].read_text()
    assert chunk.startswith("/* PGO training corpus") and "corpus_000_0000(X86 *c)" in chunk
    for function in re.findall(r"^void corpus_\w+\(X86 \*c\) \{\n(.*?)^\}", chunk, re.S | re.M):
        labels = re.findall(r"^(\w+): ;", function, re.M)
        assert len(labels) == len(set(labels))
    table = first[-1].read_text()
    assert table.count("corpus_000_0000") == 2


def test_the_pgo_corpus_compiles_freestanding(tmp_path):
    pytest.importorskip("unicorn")
    import pgo
    try:
        cc = bundle.find_tool(None, "RECOMP_FREESTANDING_CC", bundle.DEFAULT_CC)
        lld = bundle.find_tool(None, "RECOMP_LD64_LLD", bundle.DEFAULT_LLD)
    except SystemExit:
        pytest.skip("an LLVM clang and ld64.lld are not installed (brew install llvm lld)")
    sources = pgo.corpus(tmp_path / "corpus", chunks=1, chunk_bytes=32 << 10)
    linked = pgo.train(Path(cc), Path(lld), sources, tmp_path / "objects", 2)
    assert bundle.foreign(bundle.macho_undefined(linked)) == []


def test_the_manifest_records_files_modes_and_links(tmp_path):
    (tmp_path / "bin").mkdir()
    (tmp_path / "bin/llvm").write_bytes(b"binary")
    (tmp_path / "bin/llvm").chmod(0o755)
    (tmp_path / "bin/clang").symlink_to("llvm")
    (tmp_path / "MANIFEST.json").write_text("{}")
    assert stage.manifest(tmp_path) == {
        "bin/clang": {"symlink": "llvm"},
        "bin/llvm": {"sha256": hashlib.sha256(b"binary").hexdigest(), "size": 6, "mode": "755"}}


# -- a staged toolchain (RECOMP_STAGED_TOOLCHAIN) ----------------------------

@pytest.fixture(scope="module")
def staged():
    path = os.environ.get("RECOMP_STAGED_TOOLCHAIN")
    if not path:
        pytest.skip("set RECOMP_STAGED_TOOLCHAIN to a tools/toolchain/stage.py output to check it")
    if platform.system() != "Darwin":
        pytest.skip("the staged toolchain runs on macOS")
    return Path(path)


def test_the_staged_tree_is_what_its_manifest_says(staged):
    record = json.loads((staged / "MANIFEST.json").read_text())
    assert record["schema"] == "recomp-toolchain/1"
    assert stage.manifest(staged) == record["files"]
    assert (staged / "llvm/bin/clang").resolve() == (staged / "llvm/bin/llvm").resolve()


def snapshot(root):
    return {p.relative_to(root).as_posix(): (p.stat().st_size, p.stat().st_mtime_ns)
            for p in root.rglob("*") if not p.is_symlink()}


def test_a_read_only_copy_runs_anywhere_and_writes_nothing(staged, tmp_path, monkeypatch):
    """App Translocation runs an app from a read-only random path; nothing may be written into it."""
    sys.path.insert(0, str(ROOT / "tools/recomp/tests"))
    from test_freestanding import translate_doubler
    copy = tmp_path / "A Game.app (translocated)" / "toolchain"
    shutil.copytree(staged, copy, symlinks=True)
    for path in [copy] + list(copy.rglob("*")):
        if not path.is_symlink():
            path.chmod(path.stat().st_mode & ~(stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH))
    before = snapshot(copy)
    try:
        python = copy / "python/bin/python3"
        env = {"HOME": str(tmp_path), "PATH": "/usr/bin:/bin"}
        out = subprocess.run([str(python), "-I", "-B", "-c",
                              "import sys, capstone, pefile, tomllib, concurrent.futures; print(sys.prefix)"],
                             env=env, check=True, capture_output=True, text=True).stdout.strip()
        assert Path(out).resolve() == (copy / "python").resolve()
        resource = subprocess.run([str(copy / "llvm/bin/clang"), "-print-resource-dir"], env=env, check=True,
                                  capture_output=True, text=True).stdout.strip()
        assert Path(resource).resolve().parent == (copy / "llvm/lib/clang").resolve()
        gen, _ = translate_doubler(tmp_path, monkeypatch)
        result = subprocess.run([str(python), "-I", "-B", str(ROOT / "tools/recomp/bundle.py"), "--gen", str(gen),
                                 "--out", str(tmp_path / "bundles"), "--jobs", "2",
                                 "--cc", str(copy / "llvm/bin/clang"), "--lld", str(copy / "llvm/bin/ld64.lld"),
                                 "-D", "GUEST_IMAGE_BASE=0x00400000u", "-D", "GUEST_HEAP_BASE=0x01000000u",
                                 "-D", "GUEST_SIZE=0x10000000u"], env=env, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "_recomp_module_register" in bundle.macho_undefined(tmp_path / "bundles/main.bundle")
        assert snapshot(copy) == before
    finally:
        for path in [copy] + list(copy.rglob("*")):
            if not path.is_symlink():
                path.chmod(path.stat().st_mode | stat.S_IWUSR)
