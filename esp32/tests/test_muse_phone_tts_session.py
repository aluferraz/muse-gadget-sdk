# SPDX-License-Identifier: Apache-2.0
"""Exercise the production session's phone/local speech handoff."""

from pathlib import Path
import subprocess
import tempfile
import unittest
from test_muse_local_tts_session import function

ROOT = Path(__file__).resolve().parents[1]


class PhoneSpeechSession(unittest.TestCase):
    def test_admission_playback_and_fallback(self):
        source = (ROOT / "components/muse/muse_chat_session.cpp").read_text()
        harness = (
            (ROOT / "tests/muse_local_tts_session_harness.cpp")
            .read_text()
            .split("int main(")[0]
        )
        fakes = """
#define CONFIG_MUSE_PHONE_RELAY 1
#include "phone_tts/muse_phone_tts.h"
static muse_phone_tts_job_t *s_phone_tts;
static bool phone_available = true, phone_done, phone_failed;
static size_t phone_queued;
static int phone_starts, phone_releases;
muse_phone_tts_job_t *muse_phone_tts_start(const char *) {
    phone_starts++; return phone_available ? reinterpret_cast<muse_phone_tts_job_t *>(2) : nullptr;
}
size_t muse_phone_tts_read(muse_phone_tts_job_t *, int16_t *out, size_t cap) {
    size_t n = phone_queued < cap ? phone_queued : cap;
    phone_queued -= n; memset(out, 0, n * 2); return n;
}
bool muse_phone_tts_done(const muse_phone_tts_job_t *) { return phone_done && !phone_queued; }
bool muse_phone_tts_failed(const muse_phone_tts_job_t *) { return phone_failed; }
void muse_phone_tts_release(muse_phone_tts_job_t *) { phone_releases++; }
"""
        code = harness.replace(
            "// INSERT PRODUCTION FUNCTIONS HERE",
            fakes
            + function(source, "start_tts")
            + function(source, "decode_phone_tts")
            + function(source, "decode_local_tts"),
        )
        code += """
int main(int argc, char **argv) {
    assert(argc == 2); int scenario = atoi(argv[1]);
    char text[] = "A complete reply."; s_turn.texts = text;
    s_turn.msgs[0].len = strlen(text); s_turn.msgs[0].tts = TTS_QUEUED;
    if (scenario == 0) phone_available = false;
    if (scenario == 5) speech = false;
    start_tts();
    if (scenario == 5) { assert(!phone_starts && !starts && s_turn.silent); return 0; }
    if (scenario == 0) { assert(phone_starts == 1 && starts == 1 && s_local_tts); return 0; }
    assert(s_phone_tts && !s_local_tts && !starts);
    decode_phone_tts(); assert(s_phone_tts && !phone_releases && !delivered);
    if (scenario == 1) return 0;
    phone_done = true;
    if (scenario == 2) phone_queued = 512;
    if (scenario == 3) phone_failed = true;
    decode_phone_tts(); assert(!s_phone_tts && phone_releases == 1);
    if (scenario == 3) {
        assert(starts == 1 && s_local_tts && s_turn.tts_msg == 0);
        terminal = true; status = MUSE_LOCAL_TTS_COMPLETE;
        decode_local_tts(); assert(s_turn.tts_msg == -1 && releases == 1);
    } else {
        assert(!starts && s_turn.tts_msg == -1 && s_turn.msgs[0].tts == TTS_FINISHED);
        assert(delivered == (scenario == 2 ? 512U : 0U));
    }
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / "session.cpp").write_text(code)
            binary = path / "session"
            subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(ROOT / "components/muse"),
                    str(path / "session.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            for scenario in range(6):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), str(scenario)], check=True)
