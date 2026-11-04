# SPDX-License-Identifier: Apache-2.0
"""Pinned vendored code and model data must remain byte-identical."""

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LocalTtsVendor(unittest.TestCase):
    def test_pinned_inventory_and_checksums(self):
        spec = importlib.util.spec_from_file_location(
            "tts_vendor_check", ROOT / "tools/muse/check_local_tts_vendor.py"
        )
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertEqual(module.check_vendor(), [])
