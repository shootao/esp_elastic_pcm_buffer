/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "elastic_pcm_buffer.h"

static const char *TAG = "elastic_basic";

#define DEMO_SAMPLE_RATE             16000
#define DEMO_CHANNELS                1
#define DEMO_BITS_PER_SAMPLE         16
#define DEMO_FRAME_MS                60
#define DEMO_FRAME_SAMPLES           ((DEMO_SAMPLE_RATE * DEMO_FRAME_MS) / 1000)
#define DEMO_FRAME_BYTES             (DEMO_FRAME_SAMPLES * DEMO_CHANNELS * (DEMO_BITS_PER_SAMPLE / 8))
#define DEMO_PUSH_COUNT              24
#define DEMO_PUSH_INTERVAL_MS        40

typedef struct {
    uint32_t callbacks;
    uint32_t callback_bytes;
    uint32_t last_sequence;
} demo_stats_t;

/** Milliseconds since boot (same time base as meta->arrival_tick_ms in elastic buffer). */
static uint32_t demo_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t demo_output_cb(void *ctx, const void *data, size_t size,
                                const elastic_pcm_buffer_frame_meta_t *meta)
{
    demo_stats_t *stats = (demo_stats_t *)ctx;
    const uint32_t t_ms = demo_now_ms();

    (void)data;

    stats->callbacks++;
    stats->callback_bytes += size;
    stats->last_sequence = meta ? meta->sequence : 0;

    ESP_LOGI(TAG,
             "[t=%" PRIu32 " ms] output cb: callbacks=%" PRIu32 " bytes=%" PRIu32 " size=%u seq=%" PRIu32
             " arrival_ms=%" PRIu32,
             t_ms,
             stats->callbacks,
             stats->callback_bytes,
             (unsigned)size,
             stats->last_sequence,
             meta ? meta->arrival_tick_ms : 0U);
    return ESP_OK;
}

void app_main(void)
{
    demo_stats_t stats = {0};
    uint8_t frame[DEMO_FRAME_BYTES];
    elastic_pcm_buffer_cfg_t cfg = ELASTIC_PCM_BUFFER_CFG_DEFAULT(DEMO_FRAME_BYTES);
    elastic_pcm_buffer_pipeline_cfg_t pipeline_cfg = {
        .sample_rate = DEMO_SAMPLE_RATE,
        .channel = DEMO_CHANNELS,
        .bits_per_sample = DEMO_BITS_PER_SAMPLE,
        .consumer_task_stack_size = 4096,
        .consumer_task_priority = 5,
        .consumer_idle_ms = 5,
        .output_cb = demo_output_cb,
        .output_ctx = &stats,
    };
    elastic_pcm_buffer_t *buffer = NULL;

    cfg.min_speed = 0.82f;
    cfg.normal_speed = 1.00f;
    cfg.max_speed = 1.18f;

    ESP_LOGI(TAG, "[t=%" PRIu32 " ms] basic pipeline example start", demo_now_ms());
    ESP_LOGI(TAG, "frame_bytes=%d frame_ms=%d push_interval_ms=%d",
             DEMO_FRAME_BYTES, DEMO_FRAME_MS, DEMO_PUSH_INTERVAL_MS);

    buffer = elastic_pcm_buffer_create(&cfg);
    ESP_ERROR_CHECK(buffer != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(elastic_pcm_buffer_set_pipeline(buffer, &pipeline_cfg));
    ESP_ERROR_CHECK(elastic_pcm_buffer_start(buffer));
    ESP_ERROR_CHECK(elastic_pcm_buffer_session_begin(buffer));

    for (uint32_t i = 0; i < DEMO_PUSH_COUNT; ++i) {
        elastic_pcm_buffer_status_t status;
        esp_err_t ret;

        memset(frame, (int)(i & 0xff), sizeof(frame));
        ret = elastic_pcm_buffer_push(buffer, frame, sizeof(frame), i);
        ESP_LOGI(TAG, "[t=%" PRIu32 " ms] push seq=%" PRIu32 " ret=%s",
                 demo_now_ms(), i, esp_err_to_name(ret));

        status = elastic_pcm_buffer_get_status(buffer);
        ESP_LOGI(TAG,
                 "[t=%" PRIu32 " ms] status level=%u/%u state=%s prefill=%s speed=%.2fx underflow=%" PRIu32
                 " overflow=%" PRIu32,
                 demo_now_ms(),
                 (unsigned)status.level,
                 (unsigned)status.capacity,
                 elastic_pcm_buffer_state_to_string(status.state),
                 status.prefill_mode ? "yes" : "no",
                 status.recommended_speed,
                 status.underflow_count,
                 status.overflow_count);

        vTaskDelay(pdMS_TO_TICKS(DEMO_PUSH_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "[t=%" PRIu32 " ms] end session with drain-direct", demo_now_ms());
    ESP_ERROR_CHECK(elastic_pcm_buffer_session_end(buffer, ELASTIC_PCM_BUFFER_STOP_MODE_DRAIN_DIRECT));

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG,
             "[t=%" PRIu32 " ms] done callbacks=%" PRIu32 " callback_bytes=%" PRIu32 " last_seq=%" PRIu32,
             demo_now_ms(),
             stats.callbacks,
             stats.callback_bytes,
             stats.last_sequence);

    elastic_pcm_buffer_destroy(buffer);
    ESP_LOGI(TAG, "[t=%" PRIu32 " ms] basic pipeline example finished", demo_now_ms());
}
