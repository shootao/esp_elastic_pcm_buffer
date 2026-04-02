/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief  Elastic PCM buffer internal state.
 *
 *         These states are derived from the current buffer level and are mainly used
 *         for status observation and adaptive playback decisions.
 */
typedef enum {
    ELASTIC_PCM_BUFFER_STATE_PREFILLING = 0,
    ELASTIC_PCM_BUFFER_STATE_NORMAL,
    ELASTIC_PCM_BUFFER_STATE_LOW_WATER,
    ELASTIC_PCM_BUFFER_STATE_HIGH_WATER,
    ELASTIC_PCM_BUFFER_STATE_UNDERFLOW,
} elastic_pcm_buffer_state_t;

/**
 * @brief  Basic elastic PCM buffer configuration.
 *
 *         `capacity` and `frame_size` define the storage layout.
 *         `start/low/target/high` define the watermark thresholds.
 *         `min/normal/max_speed` define the recommended sonic speed range.
 */
typedef struct {
    size_t  capacity;
    size_t  frame_size;
    size_t  start_watermark;
    size_t  low_watermark;
    size_t  target_watermark;
    size_t  high_watermark;
    float   min_speed;
    float   normal_speed;
    float   max_speed;
} elastic_pcm_buffer_cfg_t;

/**
 * @brief  Default elastic PCM buffer configuration helper.
 *
 *         This macro fills a commonly used default policy:
 *         - capacity = 16 frames
 *         - start watermark = 6
 *         - low watermark = 4
 *         - target watermark = 8
 *         - high watermark = 12
 *         - sonic speed range = 0.92x ~ 1.08x
 *
 *         `frame_size_bytes` must be provided by the caller, because it depends
 *         on the PCM format used by the actual application.
 *
 *         Example:
 *         `elastic_pcm_buffer_cfg_t cfg = ELASTIC_PCM_BUFFER_CFG_DEFAULT(1920);`
 */
#define ELASTIC_PCM_BUFFER_CFG_DEFAULT(frame_size_bytes) {  \
    .capacity         = 16,                                 \
    .frame_size       = (frame_size_bytes),                 \
    .start_watermark  = 6,                                  \
    .low_watermark    = 4,                                  \
    .target_watermark = 8,                                  \
    .high_watermark   = 12,                                 \
    .min_speed        = 0.92f,                              \
    .normal_speed     = 1.00f,                              \
    .max_speed        = 1.08f,                              \
}

/**
 * @brief  Metadata attached to one PCM frame stored in the buffer.
 *
 *         This structure does not contain PCM data itself. It only describes one
 *         packet/frame:
 * - `sequence`: frame sequence number
 * - `arrival_tick_ms`: local arrival timestamp in milliseconds
 * - `size`: actual payload size of the stored frame
 */
typedef struct {
    uint32_t  sequence;
    uint32_t  arrival_tick_ms;
    size_t    size;
} elastic_pcm_buffer_frame_meta_t;

/**
 * @brief  Snapshot of the runtime status.
 *
 *         This is intended for status logging, debug, and adaptive policy decisions.
 *         `recommended_speed` is the speed value suggested by the current water level.
 */
typedef struct {
    size_t                      level;
    size_t                      capacity;
    size_t                      start_watermark;
    size_t                      low_watermark;
    size_t                      target_watermark;
    size_t                      high_watermark;
    bool                        prefill_mode;
    elastic_pcm_buffer_state_t  state;
    float                       recommended_speed;
    uint32_t                    pushed_packets;
    uint32_t                    popped_packets;
    uint32_t                    underflow_count;
    uint32_t                    overflow_count;
} elastic_pcm_buffer_status_t;

/**
 * @brief  Output callback used by pipeline mode.
 *
 *         In pipeline mode, the elastic PCM buffer owns an internal consumer task.
 *         When one frame is ready for output, the module calls this callback with
 *         processed audio data:
 * - normal mode: data may be adjusted by sonic
 * - drain-direct mode: data is passed through directly
 */
typedef esp_err_t (*elastic_pcm_buffer_output_cb_t)(void *ctx, const void *data, size_t size,
                                                    const elastic_pcm_buffer_frame_meta_t *meta);

/**
 * @brief  Optional notifications for buffer health (starvation, overflow, manual pop underflow).
 *
 *         Use `elastic_pcm_buffer_set_event_handler()` to register. Callback thread context:
 *         - ELASTIC_PCM_BUFFER_EVENT_CONSUMER_STARVED / RECOVERED: internal consumer task
 *         - ELASTIC_PCM_BUFFER_EVENT_PUSH_OVERFLOW: caller of `elastic_pcm_buffer_push()`
 *         - ELASTIC_PCM_BUFFER_EVENT_POP_UNDERFLOW: caller of `elastic_pcm_buffer_pop()`
 *
 *         Keep the callback short; do not block or call back into the same buffer in a way
 *         that deadlocks (e.g. waiting on another task that waits on this buffer).
 */
typedef enum {
    /** Pipeline consumer had no frame (prefill, empty, or below start watermark). Emitted once per gap. */
    ELASTIC_PCM_BUFFER_EVENT_CONSUMER_STARVED = 0,
    /** Consumer got a frame again after a prior CONSUMER_STARVED episode. */
    ELASTIC_PCM_BUFFER_EVENT_CONSUMER_RECOVERED,
    /** `elastic_pcm_buffer_pop()` called while the queue was empty. */
    ELASTIC_PCM_BUFFER_EVENT_POP_UNDERFLOW,
    /** `elastic_pcm_buffer_push()` called while the queue was full. */
    ELASTIC_PCM_BUFFER_EVENT_PUSH_OVERFLOW,
} elastic_pcm_buffer_event_t;

/**
 * @brief  Event payload passed to `elastic_pcm_buffer_event_cb_t`.
 */
typedef struct {
    elastic_pcm_buffer_event_t   type;
    elastic_pcm_buffer_status_t  status;
} elastic_pcm_buffer_event_info_t;

typedef void (*elastic_pcm_buffer_event_cb_t)(void *ctx, const elastic_pcm_buffer_event_info_t *info);

/**
 * @brief  Stop behavior for pipeline mode.
 */
typedef enum {
    /**
     * @brief  Drop all queued frames immediately.
     *
     *         Suitable when the current audio session is finished and any remaining
     *         tail audio should be discarded.
     */
    ELASTIC_PCM_BUFFER_STOP_MODE_DISCARD = 0,

    /**
     * @brief  Drain queued frames directly without watermark control or sonic.
     *
     *         Suitable when the current session is ending but the remaining queued
     *         audio should still be played out.
     */
    ELASTIC_PCM_BUFFER_STOP_MODE_DRAIN_DIRECT,
} elastic_pcm_buffer_stop_mode_t;

/**
 * @brief  Configuration for high-level pipeline mode.
 *
 *         In this mode, the elastic PCM buffer owns an internal consumer task and
 *         outputs audio through `output_cb`. The upper layer only needs to push
 *         input frames.
 */
typedef struct {
    uint32_t                        sample_rate;
    uint8_t                         channel;
    uint8_t                         bits_per_sample;
    uint32_t                        consumer_task_stack_size;
    uint8_t                         consumer_task_priority;
    uint32_t                        consumer_idle_ms;
    elastic_pcm_buffer_output_cb_t  output_cb;
    void                           *output_ctx;
} elastic_pcm_buffer_pipeline_cfg_t;

typedef struct elastic_pcm_buffer elastic_pcm_buffer_t;

/**
 * @brief  Create an elastic PCM buffer instance.
 *
 *         This allocates the ring buffer storage and initializes internal state.
 *         After creation, you may use either:
 * - low-level mode: `elastic_pcm_buffer_push()` + `elastic_pcm_buffer_pop()`
 * - high-level mode: `elastic_pcm_buffer_set_pipeline()` +
 *   `elastic_pcm_buffer_start()` + `elastic_pcm_buffer_push()`
 *
 * @param[in]  cfg  Buffer configuration.
 *
 * @return
 *       - non-NULL  success
 *       - NULL      invalid argument or memory allocation failure
 */
elastic_pcm_buffer_t *elastic_pcm_buffer_create(const elastic_pcm_buffer_cfg_t *cfg);

/**
 * @brief  Destroy an elastic PCM buffer instance.
 *
 *         If pipeline mode is active, the internal consumer task will be stopped first.
 *
 * @param[in]  buffer  Elastic PCM buffer handle.
 */
void elastic_pcm_buffer_destroy(elastic_pcm_buffer_t *buffer);

/**
 * @brief  Register (or clear) an optional event callback.
 *
 *         Pass NULL for `cb` to disable events. May be called before or after
 *         `elastic_pcm_buffer_set_pipeline()` / `elastic_pcm_buffer_start()`.
 *
 * @param[in]  buffer  Elastic PCM buffer handle.
 * @param[in]  cb      Callback, or NULL.
 * @param[in]  ctx     User context passed to `cb`.
 */
void elastic_pcm_buffer_set_event_handler(elastic_pcm_buffer_t *buffer,
                                          elastic_pcm_buffer_event_cb_t cb, void *ctx);

/**
 * @brief  Configure the output pipeline for high-level mode.
 *
 *         This does not start the internal consumer task by itself.
 *         Call `elastic_pcm_buffer_start()` after this function succeeds.
 *
 *         Typical high-level usage:
 *         1. `elastic_pcm_buffer_create()`
 *         2. `elastic_pcm_buffer_set_pipeline()`
 *         3. `elastic_pcm_buffer_start()`
 *         4. upper layer continuously calls `elastic_pcm_buffer_push()`
 *
 * @param[in]  buffer  Elastic PCM buffer handle.
 * @param[in]  cfg     Pipeline configuration, must include a valid `output_cb`.
 *
 * @return
 *       - ESP_OK                 success
 *       - ESP_ERR_INVALID_ARG    invalid parameter
 *       - ESP_ERR_INVALID_STATE  pipeline already started
 */
esp_err_t elastic_pcm_buffer_set_pipeline(elastic_pcm_buffer_t *buffer,
                                          const elastic_pcm_buffer_pipeline_cfg_t *cfg);

/**
 * @brief  Start pipeline mode.
 *
 *         This creates the internal consumer task. After startup, the module consumes
 *         queued frames internally and outputs processed audio through `output_cb`.
 *
 * @param[in]  buffer  Elastic PCM buffer handle.
 *
 * @return
 *       - ESP_OK                 success
 *       - ESP_ERR_INVALID_ARG    invalid parameter
 *       - ESP_ERR_INVALID_STATE  pipeline is already started or not configured
 *       - ESP_ERR_NO_MEM         internal allocation or task creation failed
 */
esp_err_t elastic_pcm_buffer_start(elastic_pcm_buffer_t *buffer);

/**
 * @brief  Prepare the elastic PCM buffer for a new audio session.
 *
 *         This API is intended for session-based real-time audio use cases such as
 *         voice chat or streaming turns.
 *
 *         Typical usage:
 *         1. current session ends via `elastic_pcm_buffer_session_end()`
 *         2. wait until the previous tail is handled as needed
 *         3. call `elastic_pcm_buffer_session_begin()`
 *         4. push the next session's input frames
 *
 *         This API clears queued audio data, resets internal stop flags, resets sonic
 *         state, and returns the buffer to prefill mode for the next round.
 *
 * @param[in]  buffer  Elastic PCM buffer handle.
 *
 * @return
 *       - ESP_OK               success
 *       - ESP_ERR_INVALID_ARG  invalid parameter
 */
esp_err_t elastic_pcm_buffer_session_begin(elastic_pcm_buffer_t *buffer);

/**
 * @brief  End the current audio session with a selected tail-handling mode.
 *
 *         This API is intended for session-based control.
 *         It does not destroy the buffer instance or stop the consumer task.
 *
 * @param[in]  buffer  Elastic PCM buffer handle.
 * @param[in]  mode    End behavior for the current session.
 *
 * @return
 *       - ESP_OK                 success
 *       - ESP_ERR_INVALID_ARG    invalid parameter
 *       - ESP_ERR_INVALID_STATE  pipeline is not started
 */
esp_err_t elastic_pcm_buffer_session_end(elastic_pcm_buffer_t *buffer,
                                         elastic_pcm_buffer_stop_mode_t mode);

/**
 * @brief  Legacy alias of `elastic_pcm_buffer_session_end()`.
 *
 *         Keep this API for backward compatibility. For new session-based designs,
 *         prefer `elastic_pcm_buffer_session_end()`.
 *
 * @param[in]  buffer  Elastic PCM buffer handle.
 * @param[in]  mode    Stop behavior.
 *
 * @return
 *       - ESP_OK                 success
 *       - ESP_ERR_INVALID_ARG    invalid parameter
 *       - ESP_ERR_INVALID_STATE  pipeline is not started
 */
esp_err_t elastic_pcm_buffer_stop(elastic_pcm_buffer_t *buffer, elastic_pcm_buffer_stop_mode_t mode);

/**
 * @brief  Push one input frame into the elastic PCM buffer.
 *
 *         This is the main input API in both low-level mode and high-level pipeline mode.
 *
 * @param[in]  buffer    Elastic PCM buffer handle.
 * @param[in]  data      Input frame buffer.
 * @param[in]  size      Actual frame size in bytes, must be <= `frame_size`.
 * @param[in]  sequence  Frame sequence number.
 *
 * @return
 *       - ESP_OK               success
 *       - ESP_ERR_INVALID_ARG  invalid parameter
 *       - ESP_ERR_NO_MEM       buffer is full
 */
esp_err_t elastic_pcm_buffer_push(elastic_pcm_buffer_t *buffer,
                                  const void *data, size_t size, uint32_t sequence);

/**
 * @brief  Pop one frame from the elastic PCM buffer manually.
 *
 *         This is the low-level/manual consume API.
 *         When pipeline mode is used, upper-layer code usually does not need to call it,
 *         because the internal consumer task already handles frame consumption.
 *
 *         Keeping this API is still useful for:
 * - debug
 * - custom playback clock control
 * - low-level integration without callback-based pipeline mode
 *
 * @param[in]   buffer    Elastic PCM buffer handle.
 * @param[out]  out_data  Output frame buffer.
 * @param[in]   out_size  Output buffer size in bytes.
 * @param[out]  meta      Optional frame metadata, may be NULL.
 *
 * @return
 *       - ESP_OK                success
 *       - ESP_ERR_INVALID_ARG   invalid parameter
 *       - ESP_ERR_INVALID_SIZE  output buffer is too small
 *       - ESP_ERR_NOT_FOUND     no frame available
 */
esp_err_t elastic_pcm_buffer_pop(elastic_pcm_buffer_t *buffer,
                                 void *out_data, size_t out_size,
                                 elastic_pcm_buffer_frame_meta_t *meta);

/**
 * @brief  Flush all queued frames and return to prefill state.
 *
 *         This clears buffered audio data but does not destroy the buffer object.
 *         Suitable for stream switching, seek, session reset, or discard stop handling.
 *
 * @param[in]  buffer  Elastic PCM buffer handle.
 *
 * @return
 *       - ESP_OK               success
 *       - ESP_ERR_INVALID_ARG  invalid parameter
 */
esp_err_t elastic_pcm_buffer_flush(elastic_pcm_buffer_t *buffer);

/**
 * @brief  Check whether the elastic PCM buffer is ready for consumption.
 *
 *         This API mainly reflects the `prefill -> consumable` decision.
 *         In low-level mode, it can be used by the upper layer to decide whether
 *         manual `pop()` should start.
 *
 *         In pipeline mode, upper-layer code usually does not need to call it.
 *
 * @param[in]  buffer  Elastic PCM buffer handle.
 *
 * @return
 *       - true   buffered data is ready to be consumed
 *       - false  still pre-filling or empty
 */
bool elastic_pcm_buffer_can_consume(elastic_pcm_buffer_t *buffer);

/**
 * @brief  Get a snapshot of the current runtime status.
 *
 * @param[in]  buffer  Elastic PCM buffer handle.
 *
 * @return
 */
elastic_pcm_buffer_status_t elastic_pcm_buffer_get_status(elastic_pcm_buffer_t *buffer);

/**
 * @brief  Convert state enum to readable string.
 *
 * @param[in]  state  Elastic PCM buffer state.
 *
 * @return
 */
const char *elastic_pcm_buffer_state_to_string(elastic_pcm_buffer_state_t state);

/**
 * @brief  Short name for an event type (for logging).
 */
const char *elastic_pcm_buffer_event_to_string(elastic_pcm_buffer_event_t event);
