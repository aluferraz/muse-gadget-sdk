# SPDX-License-Identifier: Apache-2.0
"""Production TTS worker: request ownership, allocation and bounded failure."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


class LocalTtsLifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / "lifecycle"
        command = [
            *shlex.split(os.environ.get("CXX", "c++")),
            "-std=c++17",
            "-O1",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-pthread",
            "-I",
            str(ROOT / "tests/local_tts_fakes"),
            "-I",
            str(MUSE),
            "-I",
            str(MUSE / "local_tts/vendor"),
            str(ROOT / "tests/muse_local_tts_lifecycle_harness.cpp"),
            "-x",
            "c++",
            str(MUSE / "local_tts/muse_tts_text.c"),
            str(MUSE / "local_tts/muse_tts_pcm.c"),
            "-o",
            str(cls.binary),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run(
            [str(self.binary), case], capture_output=True, text=True, timeout=20
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for case in (
    "success",
    "invalid",
    "init_failure",
    "arena_failure",
    "pcm_failure",
    "engine_failure",
    "unknown_word",
    "empty",
    "nonfinite",
    "backpressure",
    "expired",
    "cancel_queued",
    "cancel_callback",
    "concurrent",
    "isolation",
    "repeat",
    "internal_fallback",
    "read_bounds",
):
    setattr(
        LocalTtsLifecycle, "test_" + case, lambda self, case=case: self.run_case(case)
    )
