/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_local_tts.h"

#include <atomic>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nano_lex_g2p.h"
#include "sanotts_model_data.h"
#include "snt_nano.h"

static const char *TAG = "muse_local_tts";
static constexpr size_t RING_BYTES = 128 * 1024;
static constexpr size_t ARENA_BYTES = 96 * 1024;
static constexpr size_t INTERNAL_RESERVE = 48 * 1024;
static constexpr size_t PHRASE_FRAMES = 16 * MUSE_LOCAL_TTS_RATE;
static constexpr size_t PHRASE_BYTES = 80;
static constexpr int64_t TIMEOUT_US = 150 * 1000000LL;

struct muse_local_tts_job {
    std::atomic<unsigned> refs{2}; /* consumer + worker */
    std::atomic<bool> cancelled{false};
    std::atomic<muse_local_tts_status_t> status{MUSE_LOCAL_TTS_RUNNING};
    StreamBufferHandle_t ring = nullptr;
    StaticStreamBuffer_t ring_control{};
    uint8_t *storage = nullptr;
    int64_t deadline = 0;
    char text[MUSE_LOCAL_TTS_TEXT_MAX + 1];
};

/* The gate protects lazy task creation and serializes admission. A worker
 * clears it only after the non-reentrant engine has returned. Pending is
 * published before the notification and consumed exactly once by the task. */
static std::atomic<bool> s_busy{false};
static std::atomic<muse_local_tts_job_t *> s_pending{nullptr};
static TaskHandle_t s_worker;

static void unref(muse_local_tts_job_t *job)
{
    if (job->refs.fetch_sub(1) != 1) {
        return;
    }
    if (job->ring) {
        vStreamBufferDelete(job->ring); /* static buffer: no FreeRTOS-owned heap */
    }
    heap_caps_free(job->storage);
    job->~muse_local_tts_job();
    heap_caps_free(job);
}

static bool stopped(const muse_local_tts_job_t *job)
{
    return job->cancelled.load() || esp_timer_get_time() >= job->deadline;
}

struct scratch_t {
    void *arena = nullptr;
    int16_t *pcm = nullptr;
    ~scratch_t()
    {
        heap_caps_free(pcm);
        heap_caps_free(arena);
    }
};

struct sink_t {
    muse_local_tts_job_t *job;
    int16_t *pcm;
    size_t frames = 0;
    muse_tts_pcm_t resampler{};
};

static int pcm_sink(const float *pcm, int n, void *context)
{
    auto *sink = static_cast<sink_t *>(context);
    if (stopped(sink->job) || n < 0 ||
        !muse_tts_pcm_append(&sink->resampler, pcm, (size_t)n, sink->pcm, PHRASE_FRAMES, &sink->frames)) {
        return 1;
    }
    /* Yield even when synthesizing to PSRAM rather than a full output ring. */
    vTaskDelay(1);
    return 0;
}

static muse_local_tts_status_t synthesize(muse_local_tts_job_t *job)
{
    if (stopped(job)) {
        return MUSE_LOCAL_TTS_FAILED;
    }
    scratch_t scratch;
    size_t available = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (available > ARENA_BYTES + INTERNAL_RESERVE) {
        scratch.arena = heap_caps_aligned_alloc(16, ARENA_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    bool internal = scratch.arena != nullptr;
    if (!scratch.arena) {
        scratch.arena = heap_caps_aligned_alloc(16, ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    scratch.pcm =
        static_cast<int16_t *>(heap_caps_malloc(PHRASE_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scratch.arena || !scratch.pcm) {
        ESP_LOGW(TAG, "no memory for synthesis");
        return MUSE_LOCAL_TTS_FAILED;
    }
    ESP_LOGI(TAG, "arena: %u bytes, %s", (unsigned)ARENA_BYTES, internal ? "internal" : "PSRAM (scalar)");

    const char *cursor = job->text;
    char phrase[PHRASE_BYTES + 1];
    int32_t ids[NANO_LEX_MAX_TOKENS];
    bool produced = false;
    while (muse_tts_phrase(&cursor, phrase, sizeof(phrase))) {
        if (stopped(job)) {
            return MUSE_LOCAL_TTS_FAILED;
        }
        int n = nano_lex_g2p_text_to_ids(phrase, ids, NANO_LEX_MAX_TOKENS);
        nano_lex_g2p_stats_t lex{};
        nano_lex_g2p_get_stats(&lex);
        /* Never knowingly speak an answer with words removed. The caller
         * retains the complete caption if the dictionary cannot say it. */
        if (n < 0 || lex.oov_words || lex.dropped) {
            ESP_LOGW(TAG, "phonemizer refused phrase: rc=%d, unknown=%u, dropped=%u", n, (unsigned)lex.oov_words,
                     (unsigned)lex.dropped);
            return MUSE_LOCAL_TTS_FAILED;
        }
        sink_t sink{job, scratch.pcm};
        snt_nano_config cfg{};
        cfg.front_blob = SANOTTS_FRONT_BLOB;
        cfg.dec_blob = SANOTTS_DEC_BLOB;
        cfg.arena = scratch.arena;
        cfg.arena_size = ARENA_BYTES;
        cfg.noise_seed = 42;
        snt_nano_stats stats{};
        int rc = snt_nano_synthesize(&cfg, ids, n, pcm_sink, &sink, &stats);
        ESP_LOGI(TAG, "phrase: rc=%d, samples=%d, arena_peak=%u, ms=%lld", rc, stats.samples,
                 (unsigned)stats.arena_peak, (long long)(stats.elapsed_us / 1000));
        if (rc || !sink.frames) {
            return MUSE_LOCAL_TTS_FAILED;
        }
        /* Publish complete phrases; scalar inference is slower than playback.
         * Every wait is bounded, and a released job can never feed a new job. */
        size_t offset = 0;
        size_t bytes = sink.frames * sizeof(int16_t);
        while (offset < bytes) {
            if (stopped(job)) {
                return MUSE_LOCAL_TTS_FAILED;
            }
            offset += xStreamBufferSend(job->ring, reinterpret_cast<uint8_t *>(scratch.pcm) + offset, bytes - offset,
                                        pdMS_TO_TICKS(20));
        }
        produced = true;
    }
    return produced ? MUSE_LOCAL_TTS_COMPLETE : MUSE_LOCAL_TTS_FAILED;
}

static void task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        auto *job = s_pending.exchange(nullptr);
        if (!job) {
            continue;
        }
        muse_local_tts_status_t result = synthesize(job);
        if (job->cancelled.load()) {
            result = MUSE_LOCAL_TTS_CANCELLED;
        }
        job->status.store(result); /* all PCM writes precede terminal status */
        s_busy.store(false);
        unref(job);
    }
}

extern "C" muse_local_tts_job_t *muse_local_tts_start(const char *input)
{
    if (!muse_tts_text_valid(input, MUSE_LOCAL_TTS_TEXT_MAX)) {
        return nullptr;
    }
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        return nullptr;
    }
    void *memory = heap_caps_malloc(sizeof(muse_local_tts_job_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        s_busy.store(false);
        return nullptr;
    }
    auto *job = new (memory) muse_local_tts_job_t;
    job->storage = static_cast<uint8_t *>(heap_caps_malloc(RING_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (job->storage) {
        job->ring = xStreamBufferCreateStatic(RING_BYTES + 1, 1, job->storage, &job->ring_control);
    }
    if (!job->ring || (!s_worker && xTaskCreatePinnedToCoreWithCaps(task, "local_tts", 16384, nullptr, 2, &s_worker, 1,
                                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)) {
        job->refs.store(1); /* never published to the worker */
        unref(job);
        s_busy.store(false);
        return nullptr;
    }
    memcpy(job->text, input, strlen(input) + 1);
    job->deadline = esp_timer_get_time() + TIMEOUT_US;
    s_pending.store(job);
    xTaskNotifyGive(s_worker);
    return job;
}

extern "C" void muse_local_tts_release(muse_local_tts_job_t *job)
{
    if (job) {
        job->cancelled.store(true);
        unref(job);
    }
}

extern "C" size_t muse_local_tts_read(muse_local_tts_job_t *job, int16_t *pcm, size_t frames)
{
    if (!job || !pcm || !frames) {
        return 0;
    }
    /* A stream buffer can publish a partial write. Read only whole samples
     * and clamp before multiplying, including for an arbitrary caller size. */
    size_t available = xStreamBufferBytesAvailable(job->ring) / sizeof(int16_t);
    size_t count = frames < available ? frames : available;
    return count ? xStreamBufferReceive(job->ring, pcm, count * sizeof(int16_t), 0) / sizeof(int16_t) : 0;
}

extern "C" muse_local_tts_status_t muse_local_tts_status(const muse_local_tts_job_t *job)
{
    return job ? job->status.load() : MUSE_LOCAL_TTS_FAILED;
}

extern "C" bool muse_local_tts_done(const muse_local_tts_job_t *job)
{
    return !job ||
           (job->status.load() != MUSE_LOCAL_TTS_RUNNING && xStreamBufferBytesAvailable(job->ring) < sizeof(int16_t));
}
