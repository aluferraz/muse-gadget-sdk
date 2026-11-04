#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify pinned local-TTS source/model bytes without downloading or executing them."""

import hashlib
import json
from pathlib import Path
import sys

VENDOR = Path(__file__).resolve().parents[2] / "components/muse/local_tts/vendor"


def check_vendor(directory=VENDOR):
    manifest = json.loads((directory / "manifest.json").read_text())
    expected = manifest["sha256"]
    actual = {p.name for p in directory.iterdir() if p.is_file()} - {
        "manifest.json",
        "UPSTREAM.md",
    }
    errors = []
    if actual != set(expected):
        errors.append("vendored file inventory differs from the manifest")
    for name, digest in expected.items():
        if Path(name).name != name:
            errors.append("manifest contains a non-local filename")
            continue
        path = directory / name
        if (
            not path.is_file()
            or hashlib.sha256(path.read_bytes()).hexdigest() != digest
        ):
            errors.append(f"checksum mismatch: {name}")
    return errors


if __name__ == "__main__":
    failures = check_vendor()
    if failures:
        print("\n".join(failures), file=sys.stderr)
        raise SystemExit(1)
    print("Local TTS vendor inventory and checksums verified.")
