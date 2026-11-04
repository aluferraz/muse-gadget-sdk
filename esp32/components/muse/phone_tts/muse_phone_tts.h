/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct muse_phone_tts_job muse_phone_tts_job_t;
/* Session-task owned; all I/O is nonblocking. Release cancels the phone pipe. */
muse_phone_tts_job_t *muse_phone_tts_start(const char *text);
size_t muse_phone_tts_read(muse_phone_tts_job_t *job, int16_t *pcm, size_t capacity);
bool muse_phone_tts_done(const muse_phone_tts_job_t *job);
bool muse_phone_tts_failed(const muse_phone_tts_job_t *job);
void muse_phone_tts_release(muse_phone_tts_job_t *job);
