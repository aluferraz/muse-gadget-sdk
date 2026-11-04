/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RELAY_VERSION 1
#define RELAY_HEADER 8
#define RELAY_PAYLOAD 180
#define RELAY_PACKET (RELAY_HEADER + RELAY_PAYLOAD)
#define RELAY_STREAMS 4
/* One DATA packet in flight per stream/direction. ACK means socket consumed it. */
enum { RELAY_HELLO = 1, RELAY_READY, RELAY_OPEN, RELAY_OPENED, RELAY_DATA, RELAY_ACK, RELAY_CLOSE };
static inline uint16_t relay_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static inline uint32_t relay_u32(const uint8_t *p) { return relay_u16(p) | (uint32_t)relay_u16(p + 2) << 16; }
static inline void relay_header(uint8_t *p, unsigned type, uint16_t id, uint32_t epoch)
{
    p[0] = RELAY_VERSION;
    p[1] = type;
    p[2] = id;
    p[3] = id >> 8;
    for (unsigned i = 0; i < 4; i++)
        p[4 + i] = epoch >> (8 * i);
}
static inline bool relay_valid(const uint8_t *p, size_t n)
{
    if (!p || n < RELAY_HEADER || n > RELAY_PACKET || p[0] != RELAY_VERSION || !relay_u32(p + 4))
        return false;
    unsigned type = p[1];
    uint16_t id = relay_u16(p + 2);
    size_t len = n - RELAY_HEADER;
    if (type == RELAY_HELLO)
        return !id && (!len || (len == 1 && p[8] == 1));
    if (type == RELAY_READY)
        return !id && !len;
    if (!id)
        return false;
    if (type == RELAY_OPEN)
        return len == 1 && p[8] >= 1 && p[8] <= 3;
    if (type == RELAY_DATA)
        return len > 0;
    return (type == RELAY_OPENED || type == RELAY_ACK || type == RELAY_CLOSE) && !len;
}
