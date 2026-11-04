/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_phone_tts.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
extern bool muse_phone_speech_ready(void);
#define MAX_SAMPLES (16000U * 60U)
struct muse_phone_tts_job {
    int fd;
    int64_t deadline;
    uint8_t request[1025], header[9];
    size_t sent, request_size, header_size, received, encoded_size, decoded;
    uint32_t samples;
    uint8_t *audio;
    int predictor, index;
    bool connected, loaded, failed;
};
static const int steps[] = {
    7,    8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,   28,
    31,   34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,  118,
    130,  143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,  494,
    544,  598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878, 2066,
    2272, 2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int indexes[] = {-1, -1, -1, -1, 2, 4, 6, 8};
static int clamp(int n, int low, int high) { return n < low ? low : n > high ? high : n; }
static void finish_socket(muse_phone_tts_job_t *j)
{
    if (j->fd >= 0) {
        close(j->fd);
        j->fd = -1;
    }
}
static void fail(muse_phone_tts_job_t *j)
{
    j->failed = true;
    finish_socket(j);
}
static bool waiting(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }
muse_phone_tts_job_t *muse_phone_tts_start(const char *text)
{
    if (!text || !muse_phone_speech_ready())
        return NULL;
    size_t len = strnlen(text, 1024);
    if (!len || len > 1023)
        return NULL;
    muse_phone_tts_job_t *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j)
        return NULL;
    j->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    j->request[0] = len;
    j->request[1] = len >> 8;
    memcpy(j->request + 2, text, len);
    j->request_size = len + 2;
    j->deadline = esp_timer_get_time() + 420000000LL;
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(18445), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (j->fd < 0 || fcntl(j->fd, F_SETFL, O_NONBLOCK) < 0) {
        muse_phone_tts_release(j);
        return NULL;
    }
    int rc = connect(j->fd, (struct sockaddr *)&address, sizeof(address));
    if (rc && !waiting()) {
        muse_phone_tts_release(j);
        return NULL;
    }
    j->connected = rc == 0;
    return j;
}
/* Reads at most 4 KiB per poll; chat control work must not starve. */
static void poll_audio(muse_phone_tts_job_t *j)
{
    if (j->failed || j->loaded)
        return;
    if (esp_timer_get_time() >= j->deadline) {
        fail(j);
        return;
    }
    if (!j->connected) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(j->fd, &write_set);
        struct timeval now = {0};
        int rc = select(j->fd + 1, NULL, &write_set, NULL, &now);
        if (rc < 0) {
            fail(j);
            return;
        }
        if (!rc)
            return;
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(j->fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) {
            fail(j);
            return;
        }
        j->connected = true;
    }
    if (j->sent < j->request_size) {
        int n = send(j->fd, j->request + j->sent, j->request_size - j->sent, 0);
        if (n > 0)
            j->sent += n;
        else if (!n || !waiting())
            fail(j);
        return;
    }
    if (j->header_size < sizeof(j->header)) {
        int n = recv(j->fd, j->header + j->header_size, sizeof(j->header) - j->header_size, 0);
        if (n > 0)
            j->header_size += n;
        else if (!n || !waiting())
            fail(j);
        if (j->header_size != sizeof(j->header))
            return;
        if (memcmp(j->header, "CPV1", 4) || j->header[4] < 1 || j->header[4] > 3) {
            fail(j);
            return;
        }
        j->samples = (uint32_t)j->header[5] | (uint32_t)j->header[6] << 8 | (uint32_t)j->header[7] << 16 |
                     (uint32_t)j->header[8] << 24;
        if (j->header[4] == 1 || j->header[4] == 3) {
            if (j->samples) {
                fail(j);
                return;
            }
            j->loaded = true;
            finish_socket(j);
            return;
        }
        if (!j->samples || j->samples > MAX_SAMPLES) {
            fail(j);
            return;
        }
        j->encoded_size = (j->samples + 1) / 2;
        j->audio = heap_caps_malloc(j->encoded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!j->audio) {
            fail(j);
            return;
        }
    }
    size_t left = j->encoded_size - j->received;
    int n = recv(j->fd, j->audio + j->received, left > 4096 ? 4096 : left, 0);
    if (n > 0)
        j->received += n;
    else if (!n || !waiting())
        fail(j);
    if (j->received == j->encoded_size) {
        j->loaded = true;
        finish_socket(j);
    }
}
size_t muse_phone_tts_read(muse_phone_tts_job_t *j, int16_t *pcm, size_t capacity)
{
    if (!j || !pcm || !capacity)
        return 0;
    poll_audio(j);
    if (j->failed || !j->loaded)
        return 0;
    size_t count = 0;
    while (count < capacity && j->decoded < j->samples) {
        int code = (j->audio[j->decoded / 2] >> ((j->decoded & 1) * 4)) & 15;
        int step = steps[j->index];
        int delta = (step >> 3) + ((code & 4) ? step : 0) + ((code & 2) ? step >> 1 : 0) +
                    ((code & 1) ? step >> 2 : 0);
        j->predictor = clamp(j->predictor + ((code & 8) ? -delta : delta), -32768, 32767);
        j->index = clamp(j->index + indexes[code & 7], 0, 88);
        pcm[count++] = j->predictor;
        j->decoded++;
    }
    return count;
}
bool muse_phone_tts_done(const muse_phone_tts_job_t *j)
{
    return !j || j->failed || (j->loaded && j->decoded == j->samples);
}
bool muse_phone_tts_failed(const muse_phone_tts_job_t *j) { return !j || j->failed; }
void muse_phone_tts_release(muse_phone_tts_job_t *j)
{
    if (j) {
        finish_socket(j);
        free(j->audio);
        memset(j->request, 0, sizeof(j->request));
        free(j);
    }
}
