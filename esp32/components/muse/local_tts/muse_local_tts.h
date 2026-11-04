// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool muse_local_tts_start(const char *text);
void muse_local_tts_cancel(void);
size_t muse_local_tts_read(int16_t *pcm, size_t frames);
bool muse_local_tts_done(void);
bool muse_local_tts_failed(void);
/* Split bounded English phrases without dropping text or cutting UTF-8. */
size_t muse_tts_phrase(const char **cursor, char *out, size_t cap);
#ifdef __cplusplus
}
#endif
