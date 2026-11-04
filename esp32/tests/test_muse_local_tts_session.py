# SPDX-License-Identifier: Apache-2.0
"""Exercise production reply admission/playback with local speech enabled."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


def function(source, name):
    start = source.index("static void " + name + "(void)")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class LocalTtsSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (MUSE / "muse_chat_session.cpp").read_text()
        code = (
            (ROOT / "tests/muse_local_tts_session_harness.cpp")
            .read_text()
            .replace(
                "// INSERT PRODUCTION FUNCTIONS HERE",
                function(source, "start_tts") + function(source, "decode_local_tts"),
            )
        )
        (out / "session.cpp").write_text(code)
        cls.binary = out / "session"
        command = [
            *shlex.split(os.environ.get("CXX", "c++")),
            "-std=c++17",
            "-O1",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-I",
            str(MUSE),
            str(out / "session.cpp"),
            "-o",
            str(cls.binary),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run(
            [str(self.binary), str(case)], capture_output=True, text=True, timeout=10
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_preference_off_keeps_captions(self):
        self.run_case(0)

    def test_speaker_off_skips_synthesis(self):
        self.run_case(1)

    def test_busy_worker_falls_back(self):
        self.run_case(2)

    def test_wait_play_drain_and_release(self):
        self.run_case(3)

    def test_partial_failure_preserves_complete_captions(self):
        self.run_case(4)
