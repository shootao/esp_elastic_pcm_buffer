/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#include "esp_timer.h"

#include "elastic_pcm_buffer.h"
#include "audio_speed_control.h"

static const char *TAG = "ELASTIC_PCM_BUFFER";

typedef struct {
    size_t    size;
    uint32_t  sequence;
    uint32_t  arrival_tick_ms;
} elastic_pcm_buffer_slot_t;

struct elastic_pcm_buffer {
    elastic_pcm_buffer_cfg_t           cfg;
    elastic_pcm_buffer_pipeline_cfg_t  pipeline_cfg;
    uint8_t                           *storage;
    elastic_pcm_buffer_slot_t         *slots;
    uint8_t                           *output_buffer;
    size_t                             output_buffer_size;
    size_t                             head;
    size_t                             tail;
    size_t                             count;
    bool                               prefill_mode;
    bool                               pipeline_started;
    bool                               task_exit_requested;
    bool                               stop_requested;
    elastic_pcm_buffer_stop_mode_t     stop_mode;
    uint32_t                           pushed_packets;
    uint32_t                           popped_packets;
    uint32_t                           underflow_count;
    uint32_t                           overflow_count;
    bool                               consumer_starved_mark;
    elastic_pcm_buffer_event_cb_t      event_cb;
    void                              *event_ctx;
    TaskHandle_t                       consumer_task;
    bool                               consumer_task_uses_caps;
    audio_speed_control_t              speed_ctrl;
    SemaphoreHandle_t                  lock;
};

static bool elastic_pcm_buffer_psram_available(void)
{
#if CONFIG_SPIRAM
    return esp_psram_is_initialized();
#else
    return false;
#endif
}

static BaseType_t elastic_pcm_buffer_create_task(TaskFunction_t main_func,
                                                 const char *name,
                                                 uint32_t stack,
                                                 void *arg,
                                                 UBaseType_t prio,
                                                 TaskHandle_t *task_handle,
                                                 BaseType_t core_id,
                                                 bool stack_in_ext,
                                                 bool *task_uses_caps)
{
    if (task_uses_caps != NULL) {
        *task_uses_caps = false;
    }

#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    if (stack_in_ext && elastic_pcm_buffer_psram_available()) {
        BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(main_func, name, stack, arg, prio, task_handle,
                                                         core_id, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ret == pdPASS) {
            if (task_uses_caps != NULL) {
                *task_uses_caps = true;
            }
            return ret;
        }
        ESP_LOGW(TAG, "create task %s in PSRAM failed, fallback to internal RAM", name);
    }
#else
    if (stack_in_ext && elastic_pcm_buffer_psram_available()) {
        ESP_LOGW(TAG, "Make sure selected the `CONFIG_SPIRAM_BOOT_INIT` and `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` by `make menuconfig`");
    }
#endif

    return xTaskCreatePinnedToCore(main_func, name, stack, arg, prio, task_handle, core_id);
}

static void elastic_pcm_buffer_delete_current_task(bool task_uses_caps)
{
    if (task_uses_caps) {
        vTaskDeleteWithCaps(NULL);
        return;
    }
    vTaskDelete(NULL);
}

static float elastic_pcm_buffer_clampf(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static float elastic_pcm_buffer_interpolate(float min_value, float max_value, float ratio)
{
    return min_value + (max_value - min_value) * elastic_pcm_buffer_clampf(ratio, 0.0f, 1.0f);
}

static elastic_pcm_buffer_state_t elastic_pcm_buffer_classify_locked(const elastic_pcm_buffer_t *jb)
{
    if (jb->count == 0) {
        return ELASTIC_PCM_BUFFER_STATE_UNDERFLOW;
    }
    if (jb->prefill_mode) {
        return ELASTIC_PCM_BUFFER_STATE_PREFILLING;
    }
    if (jb->count < jb->cfg.low_watermark) {
        return ELASTIC_PCM_BUFFER_STATE_LOW_WATER;
    }
    if (jb->count > jb->cfg.high_watermark) {
        return ELASTIC_PCM_BUFFER_STATE_HIGH_WATER;
    }
    return ELASTIC_PCM_BUFFER_STATE_NORMAL;
}

static float elastic_pcm_buffer_recommended_speed_locked(const elastic_pcm_buffer_t *jb)
{
    size_t level = jb->count;
    size_t target = jb->cfg.target_watermark;
    size_t capacity = jb->cfg.capacity;
    size_t upper_span = (capacity > target) ? (capacity - target) : 1;

    if (jb->prefill_mode || level == 0) {
        return jb->cfg.min_speed;
    }

    if (level < target) {
        float ratio = (target > 0) ? ((float)level / (float)target) : 1.0f;
        return elastic_pcm_buffer_interpolate(jb->cfg.min_speed, jb->cfg.normal_speed, ratio);
    }

    if (level > target) {
        float ratio = (float)(level - target) / (float)upper_span;
        return elastic_pcm_buffer_interpolate(jb->cfg.normal_speed, jb->cfg.max_speed, ratio);
    }

    return jb->cfg.normal_speed;
}

static elastic_pcm_buffer_status_t elastic_pcm_buffer_status_locked(const elastic_pcm_buffer_t *jb)
{
    return (elastic_pcm_buffer_status_t) {
        .level = jb->count,
        .capacity = jb->cfg.capacity,
        .start_watermark = jb->cfg.start_watermark,
        .low_watermark = jb->cfg.low_watermark,
        .target_watermark = jb->cfg.target_watermark,
        .high_watermark = jb->cfg.high_watermark,
        .prefill_mode = jb->prefill_mode,
        .state = elastic_pcm_buffer_classify_locked(jb),
        .recommended_speed = elastic_pcm_buffer_recommended_speed_locked(jb),
        .pushed_packets = jb->pushed_packets,
        .popped_packets = jb->popped_packets,
        .underflow_count = jb->underflow_count,
        .overflow_count = jb->overflow_count,
    };
}

static void elastic_pcm_buffer_emit_event(elastic_pcm_buffer_t *jb, elastic_pcm_buffer_event_t ev)
{
    elastic_pcm_buffer_event_info_t info = {.type = ev};
    elastic_pcm_buffer_event_cb_t cb = NULL;
    void *ctx = NULL;

    if (jb == NULL) {
        return;
    }

    xSemaphoreTake(jb->lock, portMAX_DELAY);
    info.status = elastic_pcm_buffer_status_locked(jb);
    cb = jb->event_cb;
    ctx = jb->event_ctx;
    xSemaphoreGive(jb->lock);

    if (cb != NULL) {
        cb(ctx, &info);
    }
}

static void elastic_pcm_buffer_notify_consumer(elastic_pcm_buffer_t *jb)
{
    TaskHandle_t consumer_task = NULL;

    if (jb == NULL) {
        return;
    }

    xSemaphoreTake(jb->lock, portMAX_DELAY);
    consumer_task = jb->consumer_task;
    xSemaphoreGive(jb->lock);

    if (consumer_task != NULL) {
        xTaskNotifyGive(consumer_task);
    }
}

static bool elastic_pcm_buffer_can_consume_locked(elastic_pcm_buffer_t *jb)
{
    if (jb->prefill_mode) {
        if (jb->count > 0 && jb->count >= jb->cfg.start_watermark) {
            jb->prefill_mode = false;
            return true;
        }
        return false;
    }

    return (jb->count > 0);
}

static void elastic_pcm_buffer_flush_locked(elastic_pcm_buffer_t *jb)
{
    memset(jb->storage, 0, jb->cfg.capacity * jb->cfg.frame_size);
    memset(jb->slots, 0, jb->cfg.capacity * sizeof(elastic_pcm_buffer_slot_t));
    jb->head = 0;
    jb->tail = 0;
    jb->count = 0;
    jb->prefill_mode = true;
}

static esp_err_t elastic_pcm_buffer_pop_locked(elastic_pcm_buffer_t *jb, void *out_data,
                                               size_t out_size, elastic_pcm_buffer_frame_meta_t *meta)
{
    uint8_t *slot_ptr = NULL;
    elastic_pcm_buffer_slot_t *slot = NULL;

    if (jb->count == 0) {
        jb->prefill_mode = true;
        jb->underflow_count++;
        return ESP_ERR_NOT_FOUND;
    }

    slot = &jb->slots[jb->head];
    if (out_size < slot->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    slot_ptr = jb->storage + (jb->head * jb->cfg.frame_size);
    memcpy(out_data, slot_ptr, slot->size);

    if (meta != NULL) {
        meta->sequence = slot->sequence;
        meta->arrival_tick_ms = slot->arrival_tick_ms;
        meta->size = slot->size;
    }

    memset(slot_ptr, 0, slot->size);
    memset(slot, 0, sizeof(*slot));

    jb->head = (jb->head + 1) % jb->cfg.capacity;
    jb->count--;
    jb->popped_packets++;

    if (jb->count == 0) {
        jb->prefill_mode = true;
    }

    return ESP_OK;
}

static void elastic_pcm_buffer_consumer_task(void *arg)
{
    elastic_pcm_buffer_t *jb = (elastic_pcm_buffer_t *)arg;
    uint8_t *input_frame = NULL;
    bool task_uses_caps = jb->consumer_task_uses_caps;

    input_frame = calloc(1, jb->cfg.frame_size);
    if (input_frame == NULL) {
        ESP_LOGE(TAG, "failed to allocate input frame");
        xSemaphoreTake(jb->lock, portMAX_DELAY);
        jb->consumer_task = NULL;
        jb->consumer_task_uses_caps = false;
        jb->pipeline_started = false;
        xSemaphoreGive(jb->lock);
        elastic_pcm_buffer_delete_current_task(task_uses_caps);
        return;
    }

    while (true) {
        elastic_pcm_buffer_output_cb_t output_cb = NULL;
        void *output_ctx = NULL;
        elastic_pcm_buffer_frame_meta_t meta = {0};
        size_t produced_size = 0;
        float recommended_speed = 1.0f;
        bool have_frame = false;
        bool drain_direct = false;
        bool wait_for_signal = false;
        esp_err_t err = ESP_OK;

        xSemaphoreTake(jb->lock, portMAX_DELAY);

        if (jb->task_exit_requested) {
            xSemaphoreGive(jb->lock);
            break;
        }

        output_cb = jb->pipeline_cfg.output_cb;
        output_ctx = jb->pipeline_cfg.output_ctx;

        if (jb->stop_requested) {
            if (jb->stop_mode == ELASTIC_PCM_BUFFER_STOP_MODE_DISCARD) {
                elastic_pcm_buffer_flush_locked(jb);
                jb->stop_requested = false;
                xSemaphoreGive(jb->lock);
                audio_speed_control_reset(&jb->speed_ctrl);
                continue;
            }

            if (jb->count > 0) {
                err = elastic_pcm_buffer_pop_locked(jb, input_frame, jb->cfg.frame_size, &meta);
                if (err == ESP_OK) {
                    have_frame = true;
                    drain_direct = true;
                }
            } else {
                jb->stop_requested = false;
            }
            xSemaphoreGive(jb->lock);

            if (!have_frame) {
                audio_speed_control_reset(&jb->speed_ctrl);
                wait_for_signal = true;
            }
        } else {
            if (elastic_pcm_buffer_can_consume_locked(jb)) {
                recommended_speed = elastic_pcm_buffer_recommended_speed_locked(jb);
                err = elastic_pcm_buffer_pop_locked(jb, input_frame, jb->cfg.frame_size, &meta);
                if (err == ESP_OK) {
                    have_frame = true;
                }
            }
            xSemaphoreGive(jb->lock);

            if (!have_frame) {
                xSemaphoreTake(jb->lock, portMAX_DELAY);
                if (!jb->consumer_starved_mark) {
                    jb->consumer_starved_mark = true;
                    xSemaphoreGive(jb->lock);
                    elastic_pcm_buffer_emit_event(jb, ELASTIC_PCM_BUFFER_EVENT_CONSUMER_STARVED);
                } else {
                    xSemaphoreGive(jb->lock);
                }
                wait_for_signal = true;
            }

            if (!wait_for_signal) {
                xSemaphoreTake(jb->lock, portMAX_DELAY);
                if (jb->consumer_starved_mark) {
                    jb->consumer_starved_mark = false;
                    xSemaphoreGive(jb->lock);
                    elastic_pcm_buffer_emit_event(jb, ELASTIC_PCM_BUFFER_EVENT_CONSUMER_RECOVERED);
                } else {
                    xSemaphoreGive(jb->lock);
                }
            }
        }

        if (wait_for_signal) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (drain_direct) {
            if (output_cb != NULL) {
                err = output_cb(output_ctx, input_frame, meta.size, &meta);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "output callback failed in drain mode: %s", esp_err_to_name(err));
                }
            }
            continue;
        }

        err = audio_speed_control_apply(&jb->speed_ctrl, recommended_speed);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to apply sonic speed: %s", esp_err_to_name(err));
            continue;
        }

        err = audio_speed_control_process(&jb->speed_ctrl, input_frame, meta.size,
                                          jb->output_buffer, jb->output_buffer_size, &produced_size);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to process sonic: %s", esp_err_to_name(err));
            continue;
        }

        if (produced_size > 0 && output_cb != NULL) {
            err = output_cb(output_ctx, jb->output_buffer, produced_size, &meta);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "output callback failed: %s", esp_err_to_name(err));
            }
        }
    }

    free(input_frame);

    xSemaphoreTake(jb->lock, portMAX_DELAY);
    jb->consumer_task = NULL;
    jb->consumer_task_uses_caps = false;
    jb->pipeline_started = false;
    xSemaphoreGive(jb->lock);
    elastic_pcm_buffer_delete_current_task(task_uses_caps);
}

elastic_pcm_buffer_t *elastic_pcm_buffer_create(const elastic_pcm_buffer_cfg_t *cfg)
{
    elastic_pcm_buffer_t *jb = NULL;

    if (cfg == NULL) {
        return NULL;
    }
    if (cfg->capacity <= 1 || cfg->frame_size == 0) {
        return NULL;
    }
    if (cfg->start_watermark > cfg->capacity ||
        cfg->low_watermark > cfg->target_watermark ||
        cfg->target_watermark > cfg->high_watermark ||
        cfg->high_watermark > cfg->capacity) {
        return NULL;
    }

    jb = calloc(1, sizeof(*jb));
    if (jb == NULL) {
        return NULL;
    }

    jb->cfg = *cfg;
    jb->prefill_mode = true;
    jb->storage = calloc(cfg->capacity, cfg->frame_size);
    jb->slots = calloc(cfg->capacity, sizeof(elastic_pcm_buffer_slot_t));
    jb->lock = xSemaphoreCreateMutex();

    if (jb->storage == NULL || jb->slots == NULL || jb->lock == NULL) {
        elastic_pcm_buffer_destroy(jb);
        return NULL;
    }

    return jb;
}

void elastic_pcm_buffer_destroy(elastic_pcm_buffer_t *jb)
{
    if (jb == NULL) {
        return;
    }

    if (jb->consumer_task != NULL) {
        xSemaphoreTake(jb->lock, portMAX_DELAY);
        jb->task_exit_requested = true;
        xSemaphoreGive(jb->lock);
        elastic_pcm_buffer_notify_consumer(jb);

        while (jb->consumer_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    audio_speed_control_deinit(&jb->speed_ctrl);

    free(jb->storage);
    jb->storage = NULL;

    free(jb->slots);
    jb->slots = NULL;

    free(jb->output_buffer);
    jb->output_buffer = NULL;

    if (jb->lock != NULL) {
        vSemaphoreDelete(jb->lock);
        jb->lock = NULL;
    }
    free(jb);
}

esp_err_t elastic_pcm_buffer_set_pipeline(elastic_pcm_buffer_t *jb,
                                          const elastic_pcm_buffer_pipeline_cfg_t *cfg)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(jb != NULL && cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->output_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "output callback is required");
    ESP_RETURN_ON_FALSE(cfg->sample_rate > 0 && cfg->channel > 0 && cfg->bits_per_sample > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid audio format");

    xSemaphoreTake(jb->lock, portMAX_DELAY);
    if (jb->pipeline_started) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        jb->pipeline_cfg = *cfg;
    }
    xSemaphoreGive(jb->lock);

    return ret;
}

esp_err_t elastic_pcm_buffer_start(elastic_pcm_buffer_t *jb)
{
    audio_speed_control_cfg_t speed_cfg;
    uint32_t stack_size = 0;
    uint8_t priority = 0;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(jb != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    xSemaphoreTake(jb->lock, portMAX_DELAY);
    if (jb->pipeline_started || jb->pipeline_cfg.output_cb == NULL) {
        xSemaphoreGive(jb->lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (jb->output_buffer == NULL) {
        jb->output_buffer_size = jb->cfg.frame_size * 2; // Sonic max processing buffer size is 2x input sizes
        jb->output_buffer = calloc(1, jb->output_buffer_size);
        if (jb->output_buffer == NULL) {
            xSemaphoreGive(jb->lock);
            return ESP_ERR_NO_MEM;
        }
    }

    speed_cfg.sample_rate = jb->pipeline_cfg.sample_rate;
    speed_cfg.channel = jb->pipeline_cfg.channel;
    speed_cfg.bits_per_sample = jb->pipeline_cfg.bits_per_sample;
    stack_size = jb->pipeline_cfg.consumer_task_stack_size ? jb->pipeline_cfg.consumer_task_stack_size : 4096;
    priority = jb->pipeline_cfg.consumer_task_priority ? jb->pipeline_cfg.consumer_task_priority : 5;
    jb->task_exit_requested = false;
    jb->stop_requested = false;
    jb->consumer_task_uses_caps = false;
    jb->consumer_starved_mark = false;
    elastic_pcm_buffer_flush_locked(jb);
    xSemaphoreGive(jb->lock);

    ret = audio_speed_control_init(&jb->speed_ctrl, &speed_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    if (elastic_pcm_buffer_create_task(elastic_pcm_buffer_consumer_task, "elastic_pcm_buffer_consumer_task",
                                       stack_size, jb, priority, &jb->consumer_task,
                                       tskNO_AFFINITY, true, &jb->consumer_task_uses_caps) != pdPASS) {
        audio_speed_control_deinit(&jb->speed_ctrl);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(jb->lock, portMAX_DELAY);
    jb->pipeline_started = true;
    xSemaphoreGive(jb->lock);
    return ESP_OK;
}

esp_err_t elastic_pcm_buffer_session_begin(elastic_pcm_buffer_t *jb)
{
    ESP_RETURN_ON_FALSE(jb != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    xSemaphoreTake(jb->lock, portMAX_DELAY);
    elastic_pcm_buffer_flush_locked(jb);
    jb->stop_mode = ELASTIC_PCM_BUFFER_STOP_MODE_DISCARD;
    jb->stop_requested = false;
    xSemaphoreGive(jb->lock);

    return audio_speed_control_reset(&jb->speed_ctrl);
}

esp_err_t elastic_pcm_buffer_session_end(elastic_pcm_buffer_t *jb, elastic_pcm_buffer_stop_mode_t mode)
{
    ESP_RETURN_ON_FALSE(jb != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    xSemaphoreTake(jb->lock, portMAX_DELAY);
    if (!jb->pipeline_started) {
        xSemaphoreGive(jb->lock);
        return ESP_ERR_INVALID_STATE;
    }
    jb->stop_mode = mode;
    jb->stop_requested = true;
    xSemaphoreGive(jb->lock);
    elastic_pcm_buffer_notify_consumer(jb);
    return ESP_OK;
}

esp_err_t elastic_pcm_buffer_stop(elastic_pcm_buffer_t *jb, elastic_pcm_buffer_stop_mode_t mode)
{
    return elastic_pcm_buffer_session_end(jb, mode);
}

esp_err_t elastic_pcm_buffer_push(elastic_pcm_buffer_t *jb, const void *data, size_t size, uint32_t sequence)
{
    uint8_t *slot_ptr = NULL;
    elastic_pcm_buffer_slot_t *slot = NULL;

    ESP_RETURN_ON_FALSE(jb != NULL && data != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(size <= jb->cfg.frame_size, ESP_ERR_INVALID_ARG, TAG, "frame too large");

    xSemaphoreTake(jb->lock, portMAX_DELAY);

    if (jb->count == jb->cfg.capacity) {
        jb->overflow_count++;
        xSemaphoreGive(jb->lock);
        elastic_pcm_buffer_emit_event(jb, ELASTIC_PCM_BUFFER_EVENT_PUSH_OVERFLOW);
        return ESP_ERR_NO_MEM;
    }

    slot = &jb->slots[jb->tail];
    slot_ptr = jb->storage + (jb->tail * jb->cfg.frame_size);

    memcpy(slot_ptr, data, size);
    slot->size = size;
    slot->sequence = sequence;
    slot->arrival_tick_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    jb->tail = (jb->tail + 1) % jb->cfg.capacity;
    jb->count++;
    jb->pushed_packets++;

    if (jb->prefill_mode && jb->count >= jb->cfg.start_watermark) {
        jb->prefill_mode = false;
    }
    xSemaphoreGive(jb->lock);
    elastic_pcm_buffer_notify_consumer(jb);
    return ESP_OK;
}

esp_err_t elastic_pcm_buffer_pop(elastic_pcm_buffer_t *jb, void *out_data, size_t out_size,
                                 elastic_pcm_buffer_frame_meta_t *meta)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(jb != NULL && out_data != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    xSemaphoreTake(jb->lock, portMAX_DELAY);
    ret = elastic_pcm_buffer_pop_locked(jb, out_data, out_size, meta);
    xSemaphoreGive(jb->lock);
    if (ret == ESP_ERR_NOT_FOUND) {
        elastic_pcm_buffer_emit_event(jb, ELASTIC_PCM_BUFFER_EVENT_POP_UNDERFLOW);
    }
    return ret;
}

esp_err_t elastic_pcm_buffer_flush(elastic_pcm_buffer_t *jb)
{
    ESP_RETURN_ON_FALSE(jb != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    xSemaphoreTake(jb->lock, portMAX_DELAY);
    elastic_pcm_buffer_flush_locked(jb);
    jb->stop_requested = false;
    xSemaphoreGive(jb->lock);
    return ESP_OK;
}

bool elastic_pcm_buffer_can_consume(elastic_pcm_buffer_t *jb)
{
    bool ready = false;

    if (jb == NULL) {
        return false;
    }
    xSemaphoreTake(jb->lock, portMAX_DELAY);
    ready = elastic_pcm_buffer_can_consume_locked(jb);
    xSemaphoreGive(jb->lock);

    return ready;
}

elastic_pcm_buffer_status_t elastic_pcm_buffer_get_status(elastic_pcm_buffer_t *jb)
{
    elastic_pcm_buffer_status_t status = {0};

    if (jb == NULL) {
        return status;
    }
    xSemaphoreTake(jb->lock, portMAX_DELAY);
    status = elastic_pcm_buffer_status_locked(jb);
    xSemaphoreGive(jb->lock);

    return status;
}

const char *elastic_pcm_buffer_state_to_string(elastic_pcm_buffer_state_t state)
{
    switch (state) {
        case ELASTIC_PCM_BUFFER_STATE_PREFILLING:
            return "prefilling";
        case ELASTIC_PCM_BUFFER_STATE_NORMAL:
            return "normal";
        case ELASTIC_PCM_BUFFER_STATE_LOW_WATER:
            return "low_water";
        case ELASTIC_PCM_BUFFER_STATE_HIGH_WATER:
            return "high_water";
        case ELASTIC_PCM_BUFFER_STATE_UNDERFLOW:
            return "underflow";
        default:
            return "unknown";
    }
}

void elastic_pcm_buffer_set_event_handler(elastic_pcm_buffer_t *jb, elastic_pcm_buffer_event_cb_t cb, void *ctx)
{
    if (jb == NULL) {
        return;
    }
    xSemaphoreTake(jb->lock, portMAX_DELAY);
    jb->event_cb = cb;
    jb->event_ctx = ctx;
    xSemaphoreGive(jb->lock);
}

const char *elastic_pcm_buffer_event_to_string(elastic_pcm_buffer_event_t event)
{
    switch (event) {
        case ELASTIC_PCM_BUFFER_EVENT_CONSUMER_STARVED:
            return "consumer_starved";
        case ELASTIC_PCM_BUFFER_EVENT_CONSUMER_RECOVERED:
            return "consumer_recovered";
        case ELASTIC_PCM_BUFFER_EVENT_POP_UNDERFLOW:
            return "pop_underflow";
        case ELASTIC_PCM_BUFFER_EVENT_PUSH_OVERFLOW:
            return "push_overflow";
        default:
            return "unknown";
    }
}
