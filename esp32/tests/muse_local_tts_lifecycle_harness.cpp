/* SPDX-License-Identifier: Apache-2.0
 * Execute the production worker against deterministic allocation, scheduling,
 * clock, stream-buffer, and inference fakes. No device or server is needed. */
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <unordered_set>
#include <vector>

#include "local_tts/muse_local_tts.cpp"

struct FakeStream {
    size_t capacity;
    std::deque<uint8_t> bytes;
};
static std::unordered_set<void *> allocations;
static int alloc_calls, fail_alloc;
static bool fail_task, fail_ring, internal_available;
static unsigned notifications;
static int64_t now;
static void (*task_body)(void *);
static int synth_rc, synth_calls, oov;
static bool empty_pcm, stalled, bad_pcm;
static muse_local_tts_job_t *release_during_synthesis;
const unsigned char SANOTTS_FRONT_BLOB[175456] = {};
const unsigned char SANOTTS_DEC_BLOB[164224] = {};

void *heap_caps_malloc(size_t n, unsigned)
{
    if (++alloc_calls == fail_alloc)
        return nullptr;
    void *p = malloc(n);
    assert(p && allocations.insert(p).second);
    return p;
}
void *heap_caps_aligned_alloc(size_t, size_t n, unsigned caps) { return heap_caps_malloc(n, caps); }
void heap_caps_free(void *p)
{
    if (p) {
        assert(allocations.erase(p) == 1);
        free(p);
    }
}
size_t heap_caps_get_free_size(unsigned) { return internal_available ? 200 * 1024 : 32 * 1024; }
int64_t esp_timer_get_time() { return now; }
void test_log(const char *, const char *, ...) {}
StreamBufferHandle_t xStreamBufferCreateStatic(size_t n, size_t, uint8_t *, StaticStreamBuffer_t *)
{
    return fail_ring ? nullptr : new FakeStream{n - 1, {}};
}
void vStreamBufferDelete(StreamBufferHandle_t s) { delete s; }
size_t xStreamBufferSend(StreamBufferHandle_t s, const void *p, size_t n, TickType_t)
{
    size_t count = std::min(n, s->capacity - s->bytes.size());
    auto *bytes = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < count; ++i)
        s->bytes.push_back(bytes[i]);
    if (!count)
        now += 1000000; // blocked consumer makes deadline progress
    return count;
}
size_t xStreamBufferReceive(StreamBufferHandle_t s, void *p, size_t n, TickType_t)
{
    size_t count = std::min(n, s->bytes.size());
    auto *bytes = static_cast<uint8_t *>(p);
    for (size_t i = 0; i < count; ++i) {
        bytes[i] = s->bytes.front();
        s->bytes.pop_front();
    }
    return count;
}
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t s) { return s->bytes.size(); }
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *, size_t, void *, unsigned,
                                           TaskHandle_t *out, int, unsigned)
{
    if (fail_task)
        return 0;
    task_body = fn;
    *out = reinterpret_cast<void *>(1);
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t) { ++notifications; }
struct Idle {};
unsigned ulTaskNotifyTake(BaseType_t, TickType_t)
{
    if (!notifications)
        throw Idle{};
    return notifications--;
}
void vTaskDelay(TickType_t ticks) { now += ticks * 1000; }
int nano_lex_g2p_text_to_ids(const char *, int32_t *out, int)
{
    out[0] = 1;
    out[1] = 3;
    out[2] = 2;
    return 3;
}
void nano_lex_g2p_get_stats(nano_lex_g2p_stats_t *stats)
{
    *stats = {};
    stats->oov_words = oov;
}
int snt_nano_synthesize(const snt_nano_config *, const int32_t *, int, snt_nano_pcm_cb cb, void *context,
                        snt_nano_stats *stats)
{
    ++synth_calls;
    if (release_during_synthesis) {
        muse_local_tts_release(release_during_synthesis);
        release_during_synthesis = nullptr;
    }
    if (empty_pcm)
        return 0;
    std::vector<float> pcm(stalled ? 240000 : 768, .25f);
    if (bad_pcm)
        pcm[0] = __builtin_nanf("");
    int rc = cb(pcm.data(), (int)pcm.size(), context);
    stats->samples = (int)pcm.size();
    return rc ? rc : synth_rc;
}
static void run_worker()
{
    assert(task_body);
    try {
        task_body(nullptr);
    } catch (const Idle &) {
    }
}
static size_t drain(muse_local_tts_job_t *job)
{
    size_t total = 0;
    int16_t pcm[128];
    while (size_t n = muse_local_tts_read(job, pcm, 128)) {
        total += n;
        for (size_t i = 0; i < n; ++i)
            assert(pcm[i] == 7000);
    }
    return total;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *mode = argv[1];
    if (!strcmp(mode, "init_failure")) {
        for (int i = 1; i <= 2; ++i) {
            alloc_calls = 0;
            fail_alloc = i;
            assert(!muse_local_tts_start("Hello."));
            assert(allocations.empty());
        }
        fail_alloc = 0;
        fail_ring = true;
        assert(!muse_local_tts_start("Hello."));
        assert(allocations.empty());
        fail_ring = false;
        fail_task = true;
        assert(!muse_local_tts_start("Hello."));
        assert(allocations.empty());
        fail_task = false;
    } else if (!strcmp(mode, "invalid")) {
        assert(!muse_local_tts_start(nullptr));
        assert(!muse_local_tts_start("   "));
        assert(!muse_local_tts_start("\xc0\x80"));
        char large[1025];
        memset(large, 'x', 1024);
        large[1024] = 0;
        assert(!muse_local_tts_start(large));
        assert(!alloc_calls);
        return 0;
    } else if (!strcmp(mode, "concurrent")) {
        std::atomic<bool> go{false};
        muse_local_tts_job_t *jobs[2]{};
        auto enter = [&](int i) {
            while (!go.load()) {
            }
            jobs[i] = muse_local_tts_start("Hello.");
        };
        std::thread a(enter, 0), b(enter, 1);
        go.store(true);
        a.join();
        b.join();
        assert((jobs[0] != nullptr) != (jobs[1] != nullptr));
        auto *job = jobs[0] ? jobs[0] : jobs[1];
        muse_local_tts_release(job);
        run_worker();
        assert(allocations.empty());
        return 0;
    }
    if (!strcmp(mode, "repeat")) {
        for (int i = 0; i < 100; ++i) {
            auto *item = muse_local_tts_start("Hello.");
            assert(item);
            run_worker();
            assert(drain(item) == 512 && muse_local_tts_done(item));
            muse_local_tts_release(item);
            assert(allocations.empty());
        }
        return 0;
    }
    auto *job = muse_local_tts_start("Hello.");
    assert(job && !muse_local_tts_start("Busy."));
    if (!strcmp(mode, "cancel_queued")) {
        muse_local_tts_release(job);
        run_worker();
        assert(!synth_calls && allocations.empty());
        return 0;
    }
    if (!strcmp(mode, "cancel_callback"))
        release_during_synthesis = job;
    if (!strcmp(mode, "internal_fallback")) {
        internal_available = true;
        fail_alloc = alloc_calls + 1;
    }
    if (!strcmp(mode, "arena_failure"))
        fail_alloc = alloc_calls + 1;
    if (!strcmp(mode, "pcm_failure"))
        fail_alloc = alloc_calls + 2;
    if (!strcmp(mode, "engine_failure"))
        synth_rc = -2;
    if (!strcmp(mode, "unknown_word"))
        oov = 1;
    if (!strcmp(mode, "nonfinite"))
        bad_pcm = true;
    if (!strcmp(mode, "empty"))
        empty_pcm = true;
    if (!strcmp(mode, "backpressure"))
        stalled = true;
    if (!strcmp(mode, "expired"))
        now = 150000000;
    run_worker();
    if (!strcmp(mode, "cancel_callback")) {
        assert(allocations.empty());
        return 0;
    }
    if (!strcmp(mode, "read_bounds")) {
        int16_t samples[513]{};
        samples[512] = 1234;
        assert(muse_local_tts_read(job, nullptr, 512) == 0);
        assert(muse_local_tts_read(job, samples, 0) == 0);
        assert(muse_local_tts_read(job, samples, SIZE_MAX) == 512);
        assert(samples[512] == 1234 && muse_local_tts_done(job));
        assert(muse_local_tts_done(nullptr));
        assert(muse_local_tts_status(nullptr) == MUSE_LOCAL_TTS_FAILED);
        muse_local_tts_release(job);
        assert(allocations.empty());
        return 0;
    }
    bool success = !strcmp(mode, "init_failure") || !strcmp(mode, "isolation") || !strcmp(mode, "success") ||
                   !strcmp(mode, "internal_fallback");
    assert(muse_local_tts_status(job) == (success ? MUSE_LOCAL_TTS_COMPLETE : MUSE_LOCAL_TTS_FAILED));
    if (!strcmp(mode, "isolation")) {
        auto *next = muse_local_tts_start("Next.");
        assert(next);
        muse_local_tts_release(job); // cannot cancel the admitted next job
        run_worker();
        assert(muse_local_tts_status(next) == MUSE_LOCAL_TTS_COMPLETE);
        assert(drain(next) == 512 && muse_local_tts_done(next));
        muse_local_tts_release(next);
    } else {
        size_t n = drain(job);
        assert(n == (success ? 512 : stalled ? 65536 : 0));
        assert(muse_local_tts_done(job));
        muse_local_tts_release(job);
    }
    assert(allocations.empty());
    return 0;
}
