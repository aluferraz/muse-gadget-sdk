/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#define CONFIG_MUSE_PHONE_RELAY 1
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)TAG)
#define ESP_LOGE(...) ((void)TAG)
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_ATT_ERR_UNLIKELY 1
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 2
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 3
#define BLE_ATT_ERR_INSUFFICIENT_RES 4
#define BLE_ATT_ERR_READ_NOT_PERMITTED 5
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_CHR_F_WRITE 1
#define BLE_GATT_CHR_F_WRITE_AUTHEN 2
#define BLE_GATT_CHR_F_NOTIFY 4
#define BLE_GAP_EVENT_CONNECT 1
#define BLE_GAP_EVENT_DISCONNECT 2
#define BLE_GAP_EVENT_SUBSCRIBE 3
#define BLE_GAP_EVENT_MTU 4
#define BLE_GAP_EVENT_ENC_CHANGE 5
struct os_mbuf {
    uint16_t len;
    uint8_t data[200];
};
#define OS_MBUF_PKTLEN(m) ((m)->len)
typedef struct {
    int type;
} ble_uuid_t;
typedef struct {
    ble_uuid_t u;
    uint8_t bytes[16];
} ble_uuid128_t;
#define BLE_UUID128_INIT(...)                                                                                          \
    {                                                                                                                  \
        .bytes = {__VA_ARGS__}                                                                                         \
    }
struct ble_gatt_access_ctxt {
    int op;
    struct os_mbuf *om;
};
struct ble_gatt_chr_def {
    const ble_uuid_t *uuid;
    int (*access_cb)(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
    uint16_t *val_handle;
    int flags;
};
struct ble_gatt_svc_def {
    int type;
    const ble_uuid_t *uuid;
    struct ble_gatt_chr_def *characteristics;
};
struct ble_gap_conn_desc {
    struct {
        bool encrypted, authenticated;
    } sec_state;
};
struct ble_gap_event {
    int type;
    struct {
        int status;
        uint16_t conn_handle;
    } connect;
    struct {
        uint16_t attr_handle;
        bool cur_notify;
    } subscribe;
    struct {
        unsigned value;
    } mtu;
};
typedef struct queue {
    pthread_mutex_t lock;
    unsigned cap, size, read, count;
    uint8_t *data;
} *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned cap, unsigned size);
int xQueueSend(QueueHandle_t q, const void *p, unsigned wait);
int xQueueReceive(QueueHandle_t q, void *p, unsigned wait);
void vQueueDelete(QueueHandle_t q);
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *out);
void vTaskDelete(void *task);
void vTaskDelay(unsigned ticks);
int64_t esp_timer_get_time(void);
int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *d);
int ble_hs_mbuf_to_flat(struct os_mbuf *m, void *p, unsigned cap, uint16_t *len);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *p, unsigned size);
int ble_gatts_notify_custom(uint16_t conn, uint16_t attr, struct os_mbuf *m);
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *s);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *s);
