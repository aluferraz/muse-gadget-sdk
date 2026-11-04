/* SPDX-License-Identifier: Apache-2.0 */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_local_tts.h"
#include "nano_lex_g2p.h"
#include "sanotts_model_data.h"
#include "snt_nano.h"
#include "snt_port.h"
static int count, nonzero, abort_now;
static FILE *raw;
void snt_par_run(snt_par_fn f, int n, void *ctx) { f(0, n, ctx); }
int snt_scratch_id(void) { return 0; }
int64_t snt_now_us(void) { return 0; }
static int sink(const float *p, int n, void *u)
{
    (void)u;
    for (int i = 0; i < n; i++) {
        assert(isfinite(p[i]));
        if (fabsf(p[i]) > .001f)
            nonzero++;
        if (raw) {
            float v = fmaxf(-1, fminf(1, p[i]));
            short s = (short)(v * 28000);
            fwrite(&s, 2, 1, raw);
        }
    }
    count += n;
    return abort_now;
}
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "split")) {
        const char *original = "Hello there. A very long sentence with many words and café café café!";
        const char *cursor = original;
        char phrase[17];
        size_t loops = 0;
        while (*cursor) {
            const char *before = cursor;
            size_t n = muse_tts_phrase(&cursor, phrase, sizeof(phrase));
            assert(n && n < sizeof(phrase) && cursor > before);
            assert(!((unsigned char)*cursor >= 0x80 && (unsigned char)*cursor < 0xc0));
            assert(++loops < 20);
        }
        cursor = "**hello**\nworld";
        assert(muse_tts_phrase(&cursor, phrase, sizeof(phrase)));
        assert(!strchr(phrase, '*') && !strchr(phrase, '\n'));
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "validation")) {
        assert(muse_tts_text_valid("a", 1));
        assert(muse_tts_text_valid("caf\xc3\xa9", 5));
        assert(!muse_tts_text_valid("caf\xc3\xa9", 4));
        assert(!muse_tts_text_valid(" \t\n", 3));
        assert(!muse_tts_text_valid("\xc0\xaf", 3));
        assert(!muse_tts_text_valid("\xed\xa0\x80", 3));
        assert(!muse_tts_text_valid("\xf4\x90\x80\x80", 4));
        assert(!muse_tts_text_valid("\xe2\x82", 3));
        assert(!muse_tts_text_valid("ab", 1));
        assert(!muse_tts_text_valid(NULL, 1));
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "pcm")) {
        float in[99];
        int16_t whole[66], chunks[67];
        for (int i = 0; i < 99; i++)
            in[i] = (i - 49) / 20.f;
        muse_tts_pcm_t state = {0};
        size_t used = 0;
        assert(muse_tts_pcm_append(&state, in, 99, whole, 66, &used) && used == 66);
        for (int block = 1; block < 20; block++) {
            state = (muse_tts_pcm_t){0};
            used = 0;
            chunks[66] = 1234;
            for (int i = 0; i < 99; i += block) {
                size_t n = 99 - i < block ? 99 - i : block;
                assert(muse_tts_pcm_append(&state, in + i, n, chunks, 66, &used));
            }
            assert(used == 66 && !memcmp(whole, chunks, sizeof(whole)) && chunks[66] == 1234);
        }
        assert(whole[0] == -28000 && whole[65] == 28000);
        state = (muse_tts_pcm_t){0};
        used = 0;
        assert(!muse_tts_pcm_append(&state, in, 99, chunks, 65, &used) && used == 65);
        float bad = NAN;
        state = (muse_tts_pcm_t){0};
        used = 0;
        assert(!muse_tts_pcm_append(&state, &bad, 1, chunks, 66, &used) && used == 0);
        return 0;
    }
    int32_t ids[NANO_LEX_MAX_TOKENS];
    int n = nano_lex_g2p_text_to_ids("Hello. My voice is generated on this device.", ids, NANO_LEX_MAX_TOKENS);
    assert(n > 2 && ids[0] == 1 && ids[n - 1] == 2);
    void *arena = malloc(96 * 1024);
    assert(arena);
    snt_nano_config cfg = {.front_blob = SANOTTS_FRONT_BLOB,
                           .dec_blob = SANOTTS_DEC_BLOB,
                           .arena = arena,
                           .arena_size = 96 * 1024,
                           .noise_seed = 42};
    snt_nano_stats stats = {0};
    if (argc > 1)
        raw = fopen(argv[1], "wb");
    assert(snt_nano_synthesize(&cfg, ids, n, sink, NULL, &stats) == 0);
    if (raw)
        fclose(raw);
    raw = NULL;
    assert(count > 24000 && nonzero > 1000 && stats.samples == count && stats.arena_peak <= 96 * 1024);
    abort_now = 1;
    count = 0;
    assert(snt_nano_synthesize(&cfg, ids, n, sink, NULL, &stats) != 0);
    assert(count > 0 && count < 24000);
    free(arena);
    return 0;
}
