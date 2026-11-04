/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_LOCAL_TTS_TEXT_MAX 1023
#define MUSE_LOCAL_TTS_RATE 16000

typedef struct muse_local_tts_job muse_local_tts_job_t;

typedef enum {
    MUSE_LOCAL_TTS_RUNNING,
    MUSE_LOCAL_TTS_COMPLETE,
    MUSE_LOCAL_TTS_CANCELLED,
    MUSE_LOCAL_TTS_FAILED,
} muse_local_tts_status_t;

/* Copies a nonempty, valid UTF-8 string of at most TEXT_MAX bytes. Returns
 * NULL on invalid input, allocation failure, or a busy synthesis worker.
 * One worker serializes the non-reentrant phonemizer and neural runtime.
 * Each accepted request owns a separate audio buffer: a new request never
 * resets or reads an older request's audio. */
muse_local_tts_job_t *muse_local_tts_start(const char *text);

/* Exactly one consumer owns the returned handle. read/status/done/release
 * must be serialized by that consumer. The worker holds its own reference,
 * so release never waits for synthesis, even when a callback is pending.
 * release cancels unfinished work and invalidates the caller's handle. */
void muse_local_tts_release(muse_local_tts_job_t *job);
size_t muse_local_tts_read(muse_local_tts_job_t *job, int16_t *pcm, size_t frames);
muse_local_tts_status_t muse_local_tts_status(const muse_local_tts_job_t *job);
bool muse_local_tts_done(const muse_local_tts_job_t *job);

/* Portable helpers, also exercised by the host harness. */
bool muse_tts_text_valid(const char *text, size_t limit);
size_t muse_tts_phrase(const char **cursor, char *out, size_t cap);

typedef struct {
    unsigned phase;
    float previous;
} muse_tts_pcm_t;

/* Bounded 24 kHz float -> 16 kHz PCM conversion; phase survives chunks.
 * Returns false for non-finite input or insufficient destination capacity.
 * The caller discards the current phrase on failure. */
bool muse_tts_pcm_append(muse_tts_pcm_t *state, const float *input, size_t count, int16_t *output, size_t capacity,
                         size_t *used);

#ifdef __cplusplus
}
#endif
