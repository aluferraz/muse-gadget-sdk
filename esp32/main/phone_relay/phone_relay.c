/* SPDX-License-Identifier: Apache-2.0 */
/* Experimental BLE socket carrier. TLS, host verification and Noise stay above
 * the sockets. Only the two compiled endpoint IDs can cross this transport. */
#include "phone_relay.h"
#include "protocol.h"
#include <stdatomic.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "os/os_mbuf.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define PORT_BASE 18442
#define DEADLINE_US (30000000LL)
static const char *TAG = "phone_relay";
/* 9e390001/2/3-7a46-4c8e-a950-1a00c0de0001 */
static const ble_uuid128_t service_uuid =
    BLE_UUID128_INIT(1, 0, 0xde, 0xc0, 0, 0x1a, 0x50, 0xa9, 0x8e, 0x4c, 0x46, 0x7a, 1, 0, 0x39, 0x9e);
static const ble_uuid128_t rx_uuid =
    BLE_UUID128_INIT(1, 0, 0xde, 0xc0, 0, 0x1a, 0x50, 0xa9, 0x8e, 0x4c, 0x46, 0x7a, 2, 0, 0x39, 0x9e);
static const ble_uuid128_t tx_uuid =
    BLE_UUID128_INIT(1, 0, 0xde, 0xc0, 0, 0x1a, 0x50, 0xa9, 0x8e, 0x4c, 0x46, 0x7a, 3, 0, 0x39, 0x9e);
static uint16_t tx_handle;
static atomic_uint connection = BLE_HS_CONN_HANDLE_NONE, epoch, mtu = 23;
static atomic_bool subscribed, ready, ble_allowed, phone_speech;
static atomic_uint notifying;
static atomic_uint connection_generation;
static QueueHandle_t incoming;
typedef struct {
    uint32_t connection_generation;
    uint16_t conn, size;
    uint8_t data[RELAY_PACKET];
} packet_t;
typedef struct {
    int fd;
    uint16_t id;
    bool opened, awaiting_ack, pending_notification;
    size_t rx_len, rx_offset;
    uint8_t rx[RELAY_PAYLOAD];
    int64_t open_deadline, tx_deadline, rx_deadline;
} stream_t;
/* Worker alone owns streams and all sockets. Host callbacks only queue input. */
static stream_t streams[RELAY_STREAMS];

bool muse_phone_relay_ready(void) { return atomic_load(&ready); }
bool muse_phone_speech_ready(void) { return atomic_load(&ready) && atomic_load(&phone_speech); }
static bool secure(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    return ble_gap_conn_find(conn, &d) == 0 && d.sec_state.encrypted && d.sec_state.authenticated;
}
static int receive(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    (void)attr;
    (void)arg;
    if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR || !incoming)
        return BLE_ATT_ERR_UNLIKELY;
    if (!secure(conn))
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    packet_t p = {
        .connection_generation = atomic_load(&connection_generation), .conn = conn, .size = OS_MBUF_PKTLEN(ctx->om)};
    if (p.size > sizeof(p.data) || ble_hs_mbuf_to_flat(ctx->om, p.data, sizeof(p.data), NULL) ||
        !relay_valid(p.data, p.size))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (!atomic_load(&subscribed) || conn != atomic_load(&connection))
        return BLE_ATT_ERR_UNLIKELY;
    return xQueueSend(incoming, &p, 0) == pdTRUE ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
static int transmit_access(uint16_t c, uint16_t a, struct ble_gatt_access_ctxt *x, void *v)
{
    (void)c;
    (void)a;
    (void)x;
    (void)v;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &service_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {.uuid = &rx_uuid.u, .access_cb = receive, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_AUTHEN},
             {.uuid = &tx_uuid.u,
              .access_cb = transmit_access,
              .val_handle = &tx_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY},
             {0}}},
    {0}};
void phone_relay_register_gatt(void)
{
    atomic_store(&ble_allowed, true);
    int rc = ble_gatts_count_cfg(services);
    if (!rc)
        rc = ble_gatts_add_svcs(services);
    if (rc)
        ESP_LOGE(TAG, "GATT registration failed: %d", rc);
}
void phone_relay_stop(void)
{
    atomic_store(&ble_allowed, false);
    atomic_store(&ready, false);
    atomic_store(&epoch, 0);
    atomic_fetch_add(&connection_generation, 1);
    while (atomic_load(&notifying))
        vTaskDelay(1);
}

void phone_relay_gap_event(struct ble_gap_event *e)
{
    if (e->type == BLE_GAP_EVENT_CONNECT && !e->connect.status) {
        atomic_store(&connection, e->connect.conn_handle);
        atomic_store(&mtu, 23);
    } else if (e->type == BLE_GAP_EVENT_DISCONNECT) {
        atomic_fetch_add(&connection_generation, 1);
        atomic_store(&ready, false);
        atomic_store(&epoch, 0);
        atomic_store(&subscribed, false);
        atomic_store(&connection, BLE_HS_CONN_HANDLE_NONE);
    } else if (e->type == BLE_GAP_EVENT_SUBSCRIBE && e->subscribe.attr_handle == tx_handle) {
        atomic_store(&subscribed, e->subscribe.cur_notify);
        if (!e->subscribe.cur_notify) {
            atomic_store(&ready, false);
            atomic_store(&epoch, 0);
        }
    } else if (e->type == BLE_GAP_EVENT_MTU)
        atomic_store(&mtu, e->mtu.value);
    else if (e->type == BLE_GAP_EVENT_ENC_CHANGE && !secure(atomic_load(&connection))) {
        atomic_store(&ready, false);
        atomic_store(&epoch, 0);
        atomic_fetch_add(&connection_generation, 1);
    }
}
static bool emit(unsigned type, uint16_t id, uint32_t generation, const void *data, size_t len)
{
    atomic_fetch_add(&notifying, 1);
    uint16_t conn = atomic_load(&connection);
    bool sent = false;
    if (atomic_load(&ble_allowed) && generation && generation == atomic_load(&epoch) && atomic_load(&subscribed)) {
        uint8_t p[RELAY_PACKET];
        relay_header(p, type, id, generation);
        if (len)
            memcpy(p + RELAY_HEADER, data, len);
        struct os_mbuf *m = ble_hs_mbuf_from_flat(p, RELAY_HEADER + len);
        sent = m && ble_gatts_notify_custom(conn, tx_handle, m) == 0;
    }
    atomic_fetch_sub(&notifying, 1);
    return sent;
}
static void close_stream(stream_t *s)
{
    if (s->fd >= 0)
        close(s->fd);
    memset(s, 0, sizeof(*s));
    s->fd = -1;
}
static void reset_streams(void)
{
    for (unsigned i = 0; i < RELAY_STREAMS; i++)
        close_stream(&streams[i]);
}
static int listener(unsigned endpoint)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in a = {
        .sin_family = AF_INET, .sin_port = htons(PORT_BASE + endpoint), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, RELAY_STREAMS) || fcntl(fd, F_SETFL, O_NONBLOCK)) {
        close(fd);
        return -1;
    }
    return fd;
}
static void worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < RELAY_STREAMS; i++)
        streams[i].fd = -1;
    int listeners[3] = {listener(1), listener(2), listener(3)};
    if (listeners[0] < 0 || listeners[1] < 0 || listeners[2] < 0) {
        for (unsigned i = 0; i < 3; i++)
            if (listeners[i] >= 0)
                close(listeners[i]);
        ESP_LOGE(TAG, "loopback listener failed");
        vTaskDelete(NULL);
        return;
    }
    uint32_t generation = 0;
    uint16_t next_id = 1;
    for (;;) {
        if (generation != atomic_load(&epoch)) {
            reset_streams();
            generation = atomic_load(&epoch);
            next_id = 1;
        }
        packet_t p;
        /* Bounded work per pass: socket output and disconnects cannot starve. */
        for (unsigned count = 0; count < 16 && xQueueReceive(incoming, &p, 0) == pdTRUE; count++) {
            if (p.connection_generation != atomic_load(&connection_generation) || p.conn != atomic_load(&connection) ||
                !atomic_load(&ble_allowed) || !atomic_load(&subscribed))
                continue;
            uint32_t pe = relay_u32(p.data + 4);
            uint16_t id = relay_u16(p.data + 2);
            unsigned type = p.data[1];
            if (type == RELAY_HELLO) {
                atomic_store(&ready, false);
                reset_streams();
                atomic_store(&phone_speech, p.size == RELAY_HEADER + 1 && p.data[RELAY_HEADER] == 1);
                generation = pe;
                next_id = 1;
                atomic_store(&epoch, pe);
                if (emit(RELAY_READY, 0, pe, NULL, 0)) {
                    atomic_store(&ready, true);
                    ESP_LOGI(TAG, "authenticated phone relay ready");
                }
                continue;
            }
            if (!atomic_load(&ready) || pe != generation)
                continue;
            stream_t *s = NULL;
            for (unsigned i = 0; i < RELAY_STREAMS; i++)
                if (streams[i].fd >= 0 && streams[i].id == id)
                    s = &streams[i];
            if (!s)
                continue; /* a reply for an already closed stream */
            bool valid = true;
            if (type == RELAY_OPENED && !s->opened)
                s->opened = true;
            else if (type == RELAY_ACK && s->awaiting_ack)
                s->awaiting_ack = false;
            else if (type == RELAY_DATA && s->opened && !s->rx_len) {
                s->rx_len = p.size - RELAY_HEADER;
                s->rx_offset = 0;
                s->rx_deadline = esp_timer_get_time() + DEADLINE_US;
                memcpy(s->rx, p.data + RELAY_HEADER, s->rx_len);
            } else if (type == RELAY_CLOSE) {
                close_stream(s);
                continue;
            } else
                valid = false;
            if (!valid) {
                emit(RELAY_CLOSE, id, generation, NULL, 0);
                close_stream(s);
            }
        }
        for (unsigned endpoint = 1; endpoint <= 3; endpoint++) {
            int fd = accept(listeners[endpoint - 1], NULL, NULL);
            if (fd < 0)
                continue;
            stream_t *s = NULL;
            for (unsigned i = 0; i < RELAY_STREAMS; i++)
                if (streams[i].fd < 0) {
                    s = &streams[i];
                    break;
                }
            uint8_t target = endpoint;
            if (!s || !atomic_load(&ready) || !next_id || fcntl(fd, F_SETFL, O_NONBLOCK) ||
                !emit(RELAY_OPEN, next_id, generation, &target, 1)) {
                close(fd);
                continue;
            }
            *s = (stream_t){.fd = fd, .id = next_id++, .open_deadline = esp_timer_get_time() + DEADLINE_US};
        }
        for (unsigned i = 0; i < RELAY_STREAMS; i++) {
            stream_t *s = &streams[i];
            if (s->fd < 0)
                continue;
            bool fail = false;
            if (s->rx_len) {
                if (s->rx_offset < s->rx_len) {
                    int n = send(s->fd, s->rx + s->rx_offset, s->rx_len - s->rx_offset, 0);
                    if (n > 0)
                        s->rx_offset += n;
                    else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                        fail = true;
                }
                if (s->rx_offset == s->rx_len && emit(RELAY_ACK, s->id, generation, NULL, 0))
                    s->rx_len = s->rx_offset = 0;
            }
            if (s->opened && !s->awaiting_ack) {
                uint8_t data[RELAY_PAYLOAD];
                size_t cap = atomic_load(&mtu) - 3 - RELAY_HEADER;
                if (cap > sizeof(data))
                    cap = sizeof(data);
                /* Peek until the BLE stack owns the notification. */
                int n = recv(s->fd, data, cap, MSG_PEEK);
                if (n > 0 && !s->pending_notification) {
                    s->pending_notification = true;
                    s->tx_deadline = esp_timer_get_time() + DEADLINE_US;
                }
                if (n > 0 && emit(RELAY_DATA, s->id, generation, data, n)) {
                    s->pending_notification = false;
                    if (recv(s->fd, data, n, 0) != n)
                        fail = true;
                    s->awaiting_ack = true;
                    s->tx_deadline = esp_timer_get_time() + DEADLINE_US;
                } else if (n == 0)
                    fail = true;
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                    fail = true;
            }
            int64_t now = esp_timer_get_time();
            if ((!s->opened && now > s->open_deadline) ||
                ((s->awaiting_ack || s->pending_notification) && now > s->tx_deadline) ||
                (s->rx_len && now > s->rx_deadline))
                fail = true;
            if (fail) {
                emit(RELAY_CLOSE, s->id, generation, NULL, 0);
                close_stream(s);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5) + 1);
    }
}
bool phone_relay_init(void)
{
    incoming = xQueueCreate(32, sizeof(packet_t));
    if (!incoming)
        return false;
    if (xTaskCreate(worker, "phone_relay", 6144, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(incoming);
        incoming = NULL;
        return false;
    }
    return true;
}
