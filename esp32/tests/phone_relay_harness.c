/* SPDX-License-Identifier: Apache-2.0 */
#include "platform.h"
#include <stdio.h>
#include "../main/phone_relay/phone_relay.c"
static atomic_bool stop_worker, authenticated = true;
static atomic_llong time_offset;
static pthread_t worker_thread;
static void (*task_fn)(void *);
static QueueHandle_t notifications;
QueueHandle_t xQueueCreate(unsigned cap, unsigned size)
{
    QueueHandle_t q = calloc(1, sizeof(*q));
    assert(q);
    q->cap = cap;
    q->size = size;
    q->data = calloc(cap, size);
    assert(q->data);
    pthread_mutex_init(&q->lock, NULL);
    return q;
}
int xQueueSend(QueueHandle_t q, const void *p, unsigned wait)
{
    (void)wait;
    pthread_mutex_lock(&q->lock);
    bool ok = q->count < q->cap;
    if (ok) {
        memcpy(q->data + ((q->read + q->count) % q->cap) * q->size, p, q->size);
        q->count++;
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}
int xQueueReceive(QueueHandle_t q, void *p, unsigned wait)
{
    (void)wait;
    pthread_mutex_lock(&q->lock);
    bool ok = q->count;
    if (ok) {
        memcpy(p, q->data + q->read * q->size, q->size);
        q->read = (q->read + 1) % q->cap;
        q->count--;
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}
void vQueueDelete(QueueHandle_t q)
{
    pthread_mutex_destroy(&q->lock);
    free(q->data);
    free(q);
}
static void *launch(void *p)
{
    task_fn(p);
    return NULL;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *out)
{
    (void)name;
    (void)stack;
    (void)priority;
    (void)out;
    task_fn = fn;
    return pthread_create(&worker_thread, NULL, launch, arg) == 0;
}
void vTaskDelete(void *task)
{
    (void)task;
    pthread_exit(NULL);
}
void vTaskDelay(unsigned ticks)
{
    if (atomic_load(&stop_worker))
        pthread_exit(NULL);
    usleep(ticks * 1000);
}
int64_t esp_timer_get_time(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000 + atomic_load(&time_offset);
}
int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *d)
{
    if (conn != 1)
        return -1;
    d->sec_state.encrypted = d->sec_state.authenticated = atomic_load(&authenticated);
    return 0;
}
int ble_hs_mbuf_to_flat(struct os_mbuf *m, void *p, unsigned cap, uint16_t *len)
{
    if (m->len > cap)
        return -1;
    memcpy(p, m->data, m->len);
    if (len)
        *len = m->len;
    return 0;
}
struct os_mbuf *ble_hs_mbuf_from_flat(const void *p, unsigned size)
{
    struct os_mbuf *m = calloc(1, sizeof(*m));
    assert(size <= sizeof(m->data));
    m->len = size;
    memcpy(m->data, p, size);
    return m;
}
int ble_gatts_notify_custom(uint16_t conn, uint16_t attr, struct os_mbuf *m)
{
    (void)conn;
    (void)attr;
    bool ok = xQueueSend(notifications, m, 0);
    free(m);
    return ok ? 0 : 1;
}
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *s)
{
    assert(s);
    return 0;
}
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *s)
{
    assert(s);
    return 0;
}
static void input(unsigned type, uint16_t id, uint32_t gen, const void *bytes, size_t len)
{
    struct os_mbuf m = {.len = RELAY_HEADER + len};
    relay_header(m.data, type, id, gen);
    if (len)
        memcpy(m.data + 8, bytes, len);
    struct ble_gatt_access_ctxt c = {.op = BLE_GATT_ACCESS_OP_WRITE_CHR, .om = &m};
    assert(!receive(1, 0, &c, NULL));
}
static struct os_mbuf output(unsigned type)
{
    struct os_mbuf m;
    int64_t until = esp_timer_get_time() + 3000000;
    while (esp_timer_get_time() < until) {
        if (xQueueReceive(notifications, &m, 0)) {
            assert(m.data[1] == type);
            return m;
        }
        usleep(1000);
    }
    fprintf(stderr, "timeout waiting type %u\n", type);
    abort();
}
static int local_connect(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in a = {
        .sin_family = AF_INET, .sin_port = htons(PORT_BASE + 1), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!connect(fd, (void *)&a, sizeof(a)));
    struct timeval t = {.tv_sec = 3};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
    return fd;
}
int main(void)
{
    phone_relay_register_gatt();
    notifications = xQueueCreate(64, sizeof(struct os_mbuf));
    assert(phone_relay_init());
    struct ble_gap_event e = {.type = BLE_GAP_EVENT_CONNECT, .connect = {.status = 0, .conn_handle = 1}};
    phone_relay_gap_event(&e);
    e = (struct ble_gap_event){.type = BLE_GAP_EVENT_SUBSCRIBE,
                               .subscribe = {.attr_handle = tx_handle, .cur_notify = true}};
    phone_relay_gap_event(&e);
    struct os_mbuf bad = {.len = 8};
    relay_header(bad.data, RELAY_HELLO, 0, 1);
    struct ble_gatt_access_ctxt ctx = {.op = 1, .om = &bad};
    atomic_store(&authenticated, false);
    assert(receive(1, 0, &ctx, NULL) == BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
    atomic_store(&authenticated, true);
    input(RELAY_HELLO, 0, 42, NULL, 0);
    output(RELAY_READY);
    assert(muse_phone_relay_ready());
    int fd = local_connect();
    struct os_mbuf open = output(RELAY_OPEN);
    uint16_t id = relay_u16(open.data + 2);
    assert(open.data[8] == 1);
    input(RELAY_OPENED, id, 42, NULL, 0);
    assert(send(fd, "abcdefghijklmnopqrstuvwx", 24, 0) == 24);
    struct os_mbuf data = output(RELAY_DATA);
    assert(data.len == 20 && !memcmp(data.data + 8, "abcdefghijkl", 12));
    usleep(50000);
    struct os_mbuf extra;
    assert(!xQueueReceive(notifications, &extra, 0));
    input(RELAY_ACK, id, 42, NULL, 0);
    data = output(RELAY_DATA);
    assert(!memcmp(data.data + 8, "mnopqrstuvwx", 12));
    input(RELAY_ACK, id, 42, NULL, 0);
    input(RELAY_DATA, id, 41, "stale", 5);
    input(RELAY_DATA, id, 42, "reply", 5);
    output(RELAY_ACK);
    char buf[32];
    assert(recv(fd, buf, sizeof(buf), 0) == 5 && !memcmp(buf, "reply", 5));
    e = (struct ble_gap_event){.type = BLE_GAP_EVENT_DISCONNECT};
    phone_relay_gap_event(&e);
    assert(!muse_phone_relay_ready());
    assert(recv(fd, buf, sizeof(buf), 0) == 0);
    close(fd);
    e = (struct ble_gap_event){.type = BLE_GAP_EVENT_CONNECT, .connect = {.status = 0, .conn_handle = 1}};
    phone_relay_gap_event(&e);
    e = (struct ble_gap_event){.type = BLE_GAP_EVENT_SUBSCRIBE,
                               .subscribe = {.attr_handle = tx_handle, .cur_notify = true}};
    phone_relay_gap_event(&e);
    input(RELAY_HELLO, 0, 43, NULL, 0);
    output(RELAY_READY);
    fd = local_connect();
    open = output(RELAY_OPEN);
    id = relay_u16(open.data + 2);
    input(RELAY_OPENED, id, 43, NULL, 0);
    input(RELAY_DATA, id, 42, "old session", 11);
    input(RELAY_DATA, id, 43, "new", 3);
    output(RELAY_ACK);
    assert(recv(fd, buf, sizeof(buf), 0) == 3 && !memcmp(buf, "new", 3));
    assert(send(fd, "timeout", 7, 0) == 7);
    output(RELAY_DATA);
    atomic_store(&time_offset, 50000000);
    output(RELAY_CLOSE);
    assert(recv(fd, buf, sizeof(buf), 0) == 0);
    close(fd);
    phone_relay_stop();
    assert(!muse_phone_relay_ready());
    atomic_store(&stop_worker, true);
    pthread_join(worker_thread, NULL);
    vQueueDelete(incoming);
    vQueueDelete(notifications);
    puts("Firmware authenticated admission, MTU, credits, stale epoch and disconnect passed");
    return 0;
}
