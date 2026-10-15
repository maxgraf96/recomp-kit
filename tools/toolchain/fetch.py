"""Download pinned upstream artifacts into a cache, verified by SHA-256.

Every artifact the toolchain is built from is named in pins.json with its URL
and SHA-256. A cached file is reused only if its digest still matches; a
download is written beside its final name and renamed into place only after
its digest checks, so an interrupted or tampered download never looks valid.
"""

import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import urllib.request

HERE = Path(__file__).resolve().parent
PINS = HERE / "pins.json"


def load_pins(path=PINS):
    """The pinned artifacts: {"llvm": {...}, "python": {...}, "wheels": [...]}."""
    return json.loads(Path(path).read_text())


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(pin, cache):
    """The cached file for `pin` ({"url", "sha256"}), downloading it if needed."""
    cache = Path(cache)
    cache.mkdir(parents=True, exist_ok=True)
    name = pin.get("file") or pin["url"].rsplit("/", 1)[1]
    target = cache / name
    if target.is_file():
        if sha256_of(target) == pin["sha256"]:
            return target
        raise SystemExit("fetch: %s is cached but its SHA-256 is not the pinned %s; delete it"
                         % (target, pin["sha256"]))
    print("fetch: %s" % pin["url"], flush=True)
    handle, partial = tempfile.mkstemp(prefix=name + ".", suffix=".part", dir=cache)
    try:
        with os.fdopen(handle, "wb") as out, urllib.request.urlopen(pin["url"], timeout=60) as response:
            shutil.copyfileobj(response, out, 1 << 20)
        actual = sha256_of(partial)
        if actual != pin["sha256"]:
            raise SystemExit("fetch: %s has SHA-256 %s; pinned %s" % (pin["url"], actual, pin["sha256"]))
        os.replace(partial, target)
    finally:
        if os.path.exists(partial):
            os.unlink(partial)
    return target
