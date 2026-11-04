// SPDX-License-Identifier: Apache-2.0
#include "muse_local_tts.h"
#include <string.h>
size_t muse_tts_phrase(const char **cursor, char *out, size_t cap)
{
    if (!cursor || !*cursor || !out || cap < 5) return 0;
    const char *s = *cursor;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') ++s;
    size_t n = 0, word = 0;
    while (s[n] && n < cap - 1) {
        if (s[n] == ' ' || s[n] == '\n') word = n + 1;
        ++n;
        if (strchr(".!?;", s[n-1]) && (!s[n] || s[n] == ' ' || s[n] == '\n')) break;
    }
    if (s[n] && n == cap - 1 && word) n = word;
    while (n && ((unsigned char)s[n] & 0xc0) == 0x80) --n;
    if (!n) { out[0] = 0; *cursor = s; return 0; }
    memcpy(out, s, n); out[n] = 0;
    for (size_t i = 0; i < n; ++i) if (strchr("*#`_\n\r\t", out[i])) out[i] = ' ';
    *cursor = s + n;
    return n;
}
