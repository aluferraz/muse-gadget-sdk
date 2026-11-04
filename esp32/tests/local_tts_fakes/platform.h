/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstddef>
#include <cstdint>
struct FakeStream;
using StreamBufferHandle_t = FakeStream *;
struct StaticStreamBuffer_t {
    int unused;
};
using TaskHandle_t = void *;
using BaseType_t = int;
using TickType_t = unsigned;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) (ms)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
void *heap_caps_malloc(size_t, unsigned);
void *heap_caps_aligned_alloc(size_t, size_t, unsigned);
void heap_caps_free(void *);
size_t heap_caps_get_free_size(unsigned);
int64_t esp_timer_get_time();
void test_log(const char *, const char *, ...);
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
StreamBufferHandle_t xStreamBufferCreateStatic(size_t, size_t, uint8_t *, StaticStreamBuffer_t *);
void vStreamBufferDelete(StreamBufferHandle_t);
size_t xStreamBufferSend(StreamBufferHandle_t, const void *, size_t, TickType_t);
size_t xStreamBufferReceive(StreamBufferHandle_t, void *, size_t, TickType_t);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t);
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*)(void *), const char *, size_t, void *, unsigned, TaskHandle_t *,
                                           int, unsigned);
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(BaseType_t, TickType_t);
void vTaskDelay(TickType_t);
