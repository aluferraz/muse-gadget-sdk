# SPDX-License-Identifier: Apache-2.0
"""Run the actual English phonemizer and neural engine on the host."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TTS = ROOT / "components/muse/local_tts"
VENDOR = TTS / "vendor"
HARNESS = (ROOT / "tests/muse_local_tts_harness.c").read_text()


class LocalTts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        path = Path(cls.tmp.name)
        source = path / "test.c"
        source.write_text(HARNESS)
        cls.binary = path / "test"
        cmd = [
            *shlex.split(os.environ.get("CC", "cc")),
            "-std=c11",
            "-O1",
            "-fsanitize=address,undefined",
            "-DNANO_LEX_WITH_SILVER=0",
            "-DSNT_NANO_FAST_MATH",
            "-I",
            str(TTS),
            "-I",
            str(VENDOR),
            str(source),
            str(TTS / "muse_tts_text.c"),
            str(TTS / "muse_tts_pcm.c"),
        ]
        cmd += [
            str(VENDOR / name)
            for name in [
                "nano_lex_g2p.c",
                "nano_lex_tables.c",
                "sanotts_model_data.c",
                "snt_nano.c",
                "snt_kernels_ref.c",
            ]
        ]
        subprocess.run(
            [*cmd, "-lm", "-o", str(cls.binary)], check=True, capture_output=True
        )

    def test_neural_speech_and_abort(self):
        subprocess.run([str(self.binary)], check=True, capture_output=True, timeout=60)

    def test_phrase_boundaries(self):
        subprocess.run(
            [str(self.binary), "split"], check=True, capture_output=True, timeout=10
        )

    def test_utf8_and_input_limits(self):
        subprocess.run(
            [str(self.binary), "validation"],
            check=True,
            capture_output=True,
            timeout=10,
        )

    def test_pcm_chunk_equivalence_and_capacity(self):
        subprocess.run(
            [str(self.binary), "pcm"], check=True, capture_output=True, timeout=10
        )
