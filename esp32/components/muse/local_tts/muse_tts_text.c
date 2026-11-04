/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_local_tts.h"

#include <string.h>

static bool space(unsigned char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

bool muse_tts_text_valid(const char *text, size_t limit)
{
    if (!text) {
        return false;
    }
    bool content = false;
    size_t i = 0;
    while (i <= limit && text[i]) {
        unsigned char c = (unsigned char)text[i++];
        content |= !space(c);
        if (c < 0x80) {
            if (c < 0x20 && !space(c)) {
                return false;
            }
            continue;
        }
        unsigned tail;
        uint32_t cp, minimum;
        if (c >= 0xc2 && c <= 0xdf) {
            tail = 1;
            cp = c & 0x1f;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            tail = 2;
            cp = c & 0x0f;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            tail = 3;
            cp = c & 7;
            minimum = 0x10000;
        } else {
            return false;
        }
        while (tail--) {
            if (i >= limit) {
                return false;
            }
            c = (unsigned char)text[i++];
            if ((c & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (c & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            return false;
        }
    }
    return content && i <= limit;
}

size_t muse_tts_phrase(const char **cursor, char *out, size_t cap)
{
    if (!cursor || !*cursor || !out || cap < 5) {
        return 0;
    }
    const char *s = *cursor;
    while (space((unsigned char)*s)) {
        ++s;
    }
    size_t n = 0;
    size_t word = 0;
    while (s[n] && n < cap - 1) {
        if (space((unsigned char)s[n])) {
            word = n + 1;
        }
        ++n;
        if (strchr(".!?;", s[n - 1]) && (!s[n] || space((unsigned char)s[n]))) {
            break;
        }
    }
    if (s[n] && n == cap - 1 && word) {
        n = word;
    }
    while (n && ((unsigned char)s[n] & 0xc0) == 0x80) {
        --n;
    }
    memcpy(out, s, n);
    out[n] = 0;
    for (size_t i = 0; i < n; ++i) {
        if (strchr("*#`_\n\r\t", out[i])) {
            out[i] = ' ';
        }
    }
    *cursor = s + n;
    return n;
}
