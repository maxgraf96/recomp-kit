"""No fused multiply-add in the x87 helpers or translated x87 code.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_x87_fma.py

Builds runtime/x87.c and the translated function of every x87 operation
x87_conformance.py knows, at -O2 with the flags the build uses, and scans
the objects with tools/recomp/check_fma.py. A control compiled with
contraction on must be caught, so the scan cannot pass by finding nothing.
The build files must keep -ffp-contract=off on the generated code and the
x87 sources."""

import os
import re
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import check_fma  # noqa: E402
import x87_conformance as X  # noqa: E402

CLANG = shutil.which("clang")
pytestmark = pytest.mark.skipif(CLANG is None, reason="clang is not on PATH")


def compile_c(src, out, *flags):
    subprocess.check_call([CLANG, "-c", "-O2", "-std=c11", "-I", os.path.join(ROOT, "runtime"),
                           *flags, src, "-o", out])
    return out


def test_x87_helpers_and_translated_x87_have_no_fma(tmp_path):
    synth, _ = X.translate_ops()
    (tmp_path / "synth.c").write_text(synth)
    objs = [compile_c(os.path.join(ROOT, "runtime/x87.c"), str(tmp_path / "x87.o"), "-ffp-contract=off"),
            compile_c(str(tmp_path / "synth.c"), str(tmp_path / "synth.o"), "-ffp-contract=off"),
            compile_c(os.path.join(ROOT, "third_party/softfloat/softfloat_x87.c"),
                      str(tmp_path / "softfloat.o"), "-ffp-contract=off", "-ffreestanding")]
    assert check_fma.scan(objs) == []


def test_the_scan_sees_a_contracted_multiply_add(tmp_path):
    src = tmp_path / "control.c"
    src.write_text("double f(double a, double b, double c) { return a * b + c; }\n")
    obj = compile_c(str(src), str(tmp_path / "control.o"), "-ffp-contract=fast")
    if not check_fma.scan([obj]):
        pytest.skip("this target has no fused multiply-add to contract into")
    off = compile_c(str(src), str(tmp_path / "control-off.o"), "-ffp-contract=off")
    assert check_fma.scan([off]) == []


def test_the_build_keeps_contraction_off():
    translate = open(os.path.join(ROOT, "cmake/Translate.cmake")).read()
    assert len(re.findall(r"target_compile_options\([^)]*-ffp-contract=off", translate)) == 2
    runtime = open(os.path.join(ROOT, "runtime/CMakeLists.txt")).read()
    assert re.search(r"set_source_files_properties\(x87\.c PROPERTIES COMPILE_OPTIONS -ffp-contract=off\)",
                     runtime)
