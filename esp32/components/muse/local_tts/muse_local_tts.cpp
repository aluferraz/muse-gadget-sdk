// SPDX-License-Identifier: Apache-2.0
#include "muse_local_tts.h"
#include "nano_lex_g2p.h"
#include "sanotts_model_data.h"
#include "snt_nano.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include <atomic>
#include <cmath>
#include <cstring>

static const char *TAG = "muse_local_tts";
static constexpr size_t RING_BYTES = 128 * 1024, ARENA_BYTES = 96 * 1024;
static StreamBufferHandle_t ring;
static StaticStreamBuffer_t ring_control;
static TaskHandle_t worker;
static std::atomic<bool> busy{false}, cancelled{false}, failed{false};
static char text[1024];
static unsigned phase;
static float previous;
static int64_t deadline;
static int16_t *phrase_pcm;
static size_t phrase_frames;
static constexpr size_t PHRASE_FRAMES = 16 * 16000;

/* 24 -> 16 kHz linear interpolation. Keep phase across callback boundaries. */
static int pcm_sink(const float *pcm, int n, void *)
{
    if (cancelled.load() || esp_timer_get_time() > deadline) return 1;
    int16_t out[512]; size_t used = 0;
    for (int i = 0; i < n; ++i) {
        float v = pcm[i];
        if (phase != 1) {
            float sample = phase == 2 ? (previous + v) * .5f : v;
            if (!std::isfinite(sample)) sample = 0;
            sample = fmaxf(-1, fminf(1, sample));
            out[used++] = (int16_t)(sample * 28000);
        }
        previous = v; phase = (phase + 1) % 3;
        if (used == 512 || i == n - 1) {
            if (phrase_frames + used > PHRASE_FRAMES) return 1;
            memcpy(phrase_pcm + phrase_frames, out, used * sizeof(int16_t));
            phrase_frames += used;
            used = 0;
        }
    }
    /* Let the idle task run even when the output ring never fills. */
    vTaskDelay(1);
    return 0;
}

static void task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        void *arena = nullptr;
        const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (free_internal > ARENA_BYTES + 48 * 1024)
            arena = heap_caps_aligned_alloc(16, ARENA_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        bool internal = arena != nullptr;
        if (!arena) arena = heap_caps_aligned_alloc(16, ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "synthesis arena: %u bytes, %s", (unsigned)ARENA_BYTES, internal ? "internal" : "PSRAM (scalar)");
        phrase_pcm = (int16_t *)heap_caps_malloc(PHRASE_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!arena || !phrase_pcm) failed.store(true);
        const char *cursor = text;
        char phrase[81]; int32_t ids[NANO_LEX_MAX_TOKENS];
        deadline = esp_timer_get_time() + 150000000;
        while (arena && phrase_pcm && !cancelled.load() && muse_tts_phrase(&cursor, phrase, sizeof(phrase))) {
            int n = nano_lex_g2p_text_to_ids(phrase, ids, NANO_LEX_MAX_TOKENS);
            if (n < 0) { ESP_LOGW(TAG, "phonemizer error %d", n); failed.store(true); break; }
            phase = 0; previous = 0; phrase_frames = 0;
            snt_nano_config cfg{};
            cfg.front_blob = SANOTTS_FRONT_BLOB; cfg.dec_blob = SANOTTS_DEC_BLOB;
            cfg.arena = arena; cfg.arena_size = ARENA_BYTES; cfg.noise_seed = 42;
            snt_nano_stats stats{};
            int rc = snt_nano_synthesize(&cfg, ids, n, pcm_sink, nullptr, &stats);
            ESP_LOGI(TAG, "phrase: rc=%d, samples=%d, arena_peak=%u, ms=%lld", rc, stats.samples,
                     (unsigned)stats.arena_peak, (long long)(stats.elapsed_us / 1000));
            if (rc) { if (!cancelled.load()) failed.store(true); break; }
            // Buffer a full phrase before publishing: scalar inference in
            // PSRAM is slower than playback, otherwise words would stutter.
            size_t offset = 0, bytes = phrase_frames * sizeof(int16_t);
            while (offset < bytes && !cancelled.load() && esp_timer_get_time() < deadline)
                offset += xStreamBufferSend(ring, (char *)phrase_pcm + offset, bytes - offset, pdMS_TO_TICKS(20));
            if (offset < bytes) { if (!cancelled.load()) failed.store(true); break; }
        }
        heap_caps_free(phrase_pcm); phrase_pcm = nullptr;
        heap_caps_free(arena);
        busy.store(false); // publish completion only after all writes cease
    }
}

extern "C" bool muse_local_tts_start(const char *input)
{
    if (!input || !input[0]) return false;
    bool expected = false;
    if (!busy.compare_exchange_strong(expected, true)) return false;
    if (!worker) {
        auto *storage = (uint8_t *)heap_caps_malloc(RING_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!storage) { busy.store(false); return false; }
        ring = xStreamBufferCreateStatic(RING_BYTES + 1, 1, storage, &ring_control);
        if (!ring || xTaskCreatePinnedToCoreWithCaps(task, "local_tts", 16384, nullptr, 2, &worker, 1,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            if (ring) vStreamBufferDelete(ring);
            heap_caps_free(storage); ring = nullptr; worker = nullptr; busy.store(false); return false;
        }
    }
    xStreamBufferReset(ring);
    strlcpy(text, input, sizeof(text));
    cancelled.store(false); failed.store(false); busy.store(true);
    xTaskNotifyGive(worker);
    return true;
}
extern "C" void muse_local_tts_cancel(void) { cancelled.store(true); }
extern "C" size_t muse_local_tts_read(int16_t *pcm, size_t frames)
{ return ring ? xStreamBufferReceive(ring, pcm, frames * sizeof(int16_t), 0) / sizeof(int16_t) : 0; }
extern "C" bool muse_local_tts_done(void)
{ return !busy.load() && (!ring || xStreamBufferBytesAvailable(ring) == 0); }
extern "C" bool muse_local_tts_failed(void) { return failed.load(); }
