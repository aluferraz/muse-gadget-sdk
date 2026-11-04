/* SPDX-License-Identifier: Apache-2.0 */

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "local_tts/muse_local_tts.h"
#define CONFIG_MUSE_ON_DEVICE_TTS 1
#define TEXT_MAX 1024
#define MIC_RATE 16000
#define TEXT_CHARS_PER_S 16
#define CAPTION_LEN(text) strlen(text)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
enum { TTS_QUEUED, TTS_ACTIVE, TTS_FINISHED };
struct msg_t {
    char id[8];
    size_t len;
    uint32_t pcm_start, pcm_frames;
    int tts;
};
struct {
    int tts_msg = -1;
    int nmsgs = 1;
    uint32_t pcm_out = 0;
    unsigned gen = 1;
    bool silent = false;
    char *texts;
    msg_t msgs[1];
} s_turn;
static muse_local_tts_job_t *s_local_tts;
static std::atomic<unsigned> s_gen{1};
static bool speaker = true, speech = true, busy = false, terminal = false;
static size_t queued, delivered;
static int starts, releases;
static muse_local_tts_status_t status = MUSE_LOCAL_TTS_RUNNING;
static bool muse_settings_speaker_on() { return speaker; }
static bool muse_settings_speak_replies() { return speech; }
static void show_reply_start(const msg_t &) {}
enum { M_AUDIO };
static void mark(int) {}
static int s_out;
static size_t xStreamBufferSpacesAvailable(int) { return 2048; }
static size_t xStreamBufferSend(int, const void *, size_t bytes, int)
{
    delivered += bytes / 2;
    return bytes;
}
extern "C" muse_local_tts_job_t *muse_local_tts_start(const char *text)
{
    assert(!strcmp(text, "A complete reply."));
    ++starts;
    return busy ? nullptr : reinterpret_cast<muse_local_tts_job_t *>(1);
}
extern "C" void muse_local_tts_release(muse_local_tts_job_t *job)
{
    assert(job);
    ++releases;
}
extern "C" size_t muse_local_tts_read(muse_local_tts_job_t *job, int16_t *pcm, size_t n)
{
    assert(job);
    n = n < queued ? n : queued;
    queued -= n;
    memset(pcm, 0, n * 2);
    return n;
}
extern "C" bool muse_local_tts_done(const muse_local_tts_job_t *) { return terminal && !queued; }
extern "C" muse_local_tts_status_t muse_local_tts_status(const muse_local_tts_job_t *) { return status; }

// INSERT PRODUCTION FUNCTIONS HERE

int main(int argc, char **argv)
{
    assert(argc == 2);
    char text[] = "A complete reply.";
    s_turn.texts = text;
    s_turn.msgs[0].len = strlen(text);
    s_turn.msgs[0].tts = TTS_QUEUED;
    int scenario = atoi(argv[1]);
    if (scenario == 0)
        speech = false;
    if (scenario == 1)
        speaker = false;
    if (scenario == 2)
        busy = true;
    start_tts();
    if (scenario < 3) {
        assert(s_turn.silent && s_turn.msgs[0].tts == TTS_ACTIVE && !s_local_tts);
        assert(starts == (scenario == 2 ? 1 : 0));
        return 0;
    }
    assert(starts == 1 && s_local_tts && !s_turn.silent);
    decode_local_tts();
    assert(!releases && s_turn.msgs[0].tts == TTS_ACTIVE); // still generating
    queued = 512;
    decode_local_tts();
    assert(delivered == 512 && s_turn.pcm_out == 512 && !releases);
    terminal = true;
    status = scenario == 3 ? MUSE_LOCAL_TTS_COMPLETE : MUSE_LOCAL_TTS_FAILED;
    decode_local_tts();
    assert(releases == 1 && !s_local_tts);
    if (scenario == 3) {
        assert(s_turn.msgs[0].tts == TTS_FINISHED && s_turn.tts_msg == -1);
        assert(s_turn.msgs[0].pcm_frames == 512);
    } else {
        assert(s_turn.silent && s_turn.msgs[0].tts == TTS_ACTIVE && s_turn.tts_msg == 0);
        assert(s_turn.msgs[0].pcm_start == 512);
        assert(s_turn.msgs[0].pcm_frames == strlen(text) * MIC_RATE / TEXT_CHARS_PER_S);
    }
}
