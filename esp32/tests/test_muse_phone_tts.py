# SPDX-License-Identifier: Apache-2.0
"""Exercise the real nonblocking speech client with local sockets and sanitizers."""

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class PhoneSpeech(unittest.TestCase):
    def test_lifecycle_and_audio_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "lwip").mkdir()
            (root / "lwip/sockets.h").write_text(
                "#include <sys/socket.h>\n#include <arpa/inet.h>\n#include <unistd.h>\n"
            )
            (root / "esp_heap_caps.h").write_text("""#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_malloc(size_t n,int c){(void)c;return malloc(n);}
static inline void *heap_caps_calloc(size_t n,size_t s,int c){(void)c;return calloc(n,s);}
""")
            (root / "esp_timer.h").write_text(
                "#include <stdint.h>\nint64_t esp_timer_get_time(void);\n"
            )
            binary = root / "test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-D_DARWIN_C_SOURCE",
                    "-D_DEFAULT_SOURCE",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(root),
                    "-I",
                    str(ROOT),
                    str(ROOT / "tests/muse_phone_tts_harness.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
