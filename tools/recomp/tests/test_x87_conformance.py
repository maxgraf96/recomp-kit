"""The translated x87 against real hardware (x87_conformance.py).

    .venv/bin/python -m pytest -q tools/recomp/tests/test_x87_conformance.py

Runs the committed vectors (data/x87/), or the corpus RECOMP_X87_VECTORS and
RECOMP_X87_RESULTS name, through translated code built with the runtime's
x87, and requires every non-transcendental result to equal the hardware's
bit for bit. The transcendental distances are printed, not checked. clang
must be on PATH (a skip otherwise)."""

import os
import platform
import shutil
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import x87_conformance as X  # noqa: E402


@pytest.fixture(scope="module")
def report(tmp_path_factory):
    if shutil.which("clang") is None:
        pytest.skip("clang is not on PATH")
    if platform.system() == "Windows":
        pytest.skip("the conformance library builds as a POSIX shared library")
    rep, text = X.check(work=str(tmp_path_factory.mktemp("x87")))
    print(text.split("| op |")[0])
    return rep


def test_every_non_transcendental_vector_equals_hardware(report):
    t = report["totals"]
    assert t["n"] > 0
    assert t["diff"] == 0, "\n".join(str(e) for e in report["examples"][:10])


#: Operations x87_conformance.py can run that no hardware recording has
#: covered yet. Against the SDM model that matches the recordings everywhere
#: else, 8,264 vectors of these four agree bit for bit.
NOT_RECORDED = {"fsubr_m64", "fcom_m64", "fild_m16", "fild_m32"}


def test_every_operation_and_control_word_is_covered(report):
    """The committed subset reaches every operation the recording has, under
    every control word it used."""
    if os.environ.get("RECOMP_X87_VECTORS"):
        pytest.skip("an external corpus covers what it covers")
    ops = {op for op, _, _ in report["groups"]}
    cws = {cw for _, cw, _ in report["groups"]}
    assert ops == set(X.OPS) - NOT_RECORDED, (set(X.OPS) - NOT_RECORDED) ^ ops
    assert cws == {"027f", "007f", "037f", "0e7f", "067f", "0a7f"}
