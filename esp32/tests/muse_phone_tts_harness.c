/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
static bool available = true;
static long long clock_us;
bool muse_phone_speech_ready(void) { return available; }
#include "components/muse/phone_tts/muse_phone_tts.c"
int64_t esp_timer_get_time(void) { return clock_us; }
static int server;
static muse_phone_tts_job_t *begin(int *peer)
{
    muse_phone_tts_job_t *j = muse_phone_tts_start("Hello");
    assert(j);
    *peer = accept(server, NULL, NULL);
    assert(*peer >= 0);
    assert(fcntl(*peer, F_SETFL, O_NONBLOCK) == 0);
    int16_t pcm[16];
    for (int i = 0; i < 20; i++)
        muse_phone_tts_read(j, pcm, 16);
    uint8_t request[7];
    size_t got = 0;
    for (int i = 0; i < 1000 && got < sizeof(request); i++) {
        muse_phone_tts_read(j, pcm, 16);
        int n = recv(*peer, request + got, sizeof(request) - got, 0);
        if (n > 0)
            got += n;
        usleep(1000);
    }
    assert(got == 7);
    assert(request[0] == 5 && request[1] == 0 && !memcmp(request + 2, "Hello", 5));
    return j;
}
static void feed(muse_phone_tts_job_t *j, int peer, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        assert(send(peer, data + i, 1, 0) == 1);
        usleep(1000);
        poll_audio(j);
    }
}
int main(void)
{
    server = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    assert(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    struct sockaddr_in a = {
        .sin_family = AF_INET, .sin_port = htons(18445), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(server, (void *)&a, sizeof(a)) == 0 && listen(server, 1) == 0);
    assert(!muse_phone_tts_start(NULL));
    available = false;
    assert(!muse_phone_tts_start("Hi"));
    available = true;
    int peer;
    muse_phone_tts_job_t *j = begin(&peer);
    uint8_t good[] = {'C', 'P', 'V', '1', 2, 4, 0, 0, 0, 0x77, 0xff};
    feed(j, peer, good, sizeof(good));
    for (int i = 0; i < 1000 && !j->loaded && !j->failed; i++) {
        poll_audio(j);
        usleep(1000);
    }
    int16_t out[4];
    assert(muse_phone_tts_read(j, out, 4) == 4);
    assert(out[0] == 11 && out[1] == 41 && out[2] == -22 && out[3] == -158);
    assert(muse_phone_tts_done(j) && !muse_phone_tts_failed(j));
    muse_phone_tts_release(j);
    close(peer);
    for (int mode = 0; mode <= 3; mode++) {
        j = begin(&peer);
        uint8_t header[] = {'C', 'P', 'V', '1', mode, 0, 0, 0, 0};
        feed(j, peer, header, sizeof(header));
        assert(muse_phone_tts_done(j));
        assert(muse_phone_tts_failed(j) == (mode == 0 || mode == 2));
        muse_phone_tts_release(j);
        close(peer);
    }
    j = begin(&peer);
    good[5] = good[6] = good[7] = good[8] = 255;
    feed(j, peer, good, 9);
    assert(muse_phone_tts_failed(j) && !j->audio);
    muse_phone_tts_release(j);
    close(peer);
    j = begin(&peer);
    close(peer);
    for (int i = 0; i < 1000 && !j->failed; i++) {
        poll_audio(j);
        usleep(1000);
    }
    assert(muse_phone_tts_failed(j));
    muse_phone_tts_release(j);
    j = begin(&peer);
    clock_us = j->deadline;
    poll_audio(j);
    assert(muse_phone_tts_failed(j));
    muse_phone_tts_release(j);
    close(peer);
    j = begin(&peer);
    muse_phone_tts_release(j);
    close(peer);
    close(server);
    puts("Phone speech lifecycle, fragmented framing, ADPCM and bounds passed");
}
