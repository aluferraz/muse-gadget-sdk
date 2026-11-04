/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "sdkconfig.h"
struct ble_gap_event;
#if CONFIG_MUSE_PHONE_RELAY
static inline bool phone_relay_enabled(void) { return true; }
bool phone_relay_init(void);
void phone_relay_register_gatt(void);
/* Call before stopping NimBLE; drains in-flight notification API calls. */
void phone_relay_stop(void);
void phone_relay_gap_event(struct ble_gap_event *event);
bool muse_phone_relay_ready(void);
#else
static inline bool phone_relay_enabled(void) { return false; }
static inline bool muse_phone_relay_ready(void) { return false; }
#endif
