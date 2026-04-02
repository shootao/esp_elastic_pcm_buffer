/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t  sample_rate;
    uint8_t   channel;
    uint8_t   bits_per_sample;
} audio_speed_control_cfg_t;

typedef struct {
    float     current_speed;
    uint32_t  sample_rate;
    uint8_t   channel;
    uint8_t   bits_per_sample;
    void     *sonic_handle;
} audio_speed_control_t;

esp_err_t audio_speed_control_init(audio_speed_control_t *ctrl, const audio_speed_control_cfg_t *cfg);
void      audio_speed_control_deinit(audio_speed_control_t *ctrl);
esp_err_t audio_speed_control_reset(audio_speed_control_t *ctrl);
esp_err_t audio_speed_control_apply(audio_speed_control_t *ctrl, float speed);
float     audio_speed_control_get(const audio_speed_control_t *ctrl);
esp_err_t audio_speed_control_process(audio_speed_control_t *ctrl, const void *input, size_t input_size,
                                      void *output, size_t output_size, size_t *produced_size);
