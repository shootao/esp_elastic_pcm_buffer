/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ae_sonic.h"
#include "audio_speed_control.h"

static const char *TAG = "SONIC_CTRL";

static esp_err_t audio_speed_control_convert_err(esp_ae_err_t err)
{
    switch (err) {
        case ESP_AE_ERR_OK:
            return ESP_OK;
        case ESP_AE_ERR_INVALID_PARAMETER:
            return ESP_ERR_INVALID_ARG;
        case ESP_AE_ERR_MEM_LACK:
            return ESP_ERR_NO_MEM;
        default:
            return ESP_FAIL;
    }
}

static uint32_t audio_speed_control_bytes_per_sample(const audio_speed_control_t *ctrl)
{
    return ctrl->channel * (ctrl->bits_per_sample >> 3);
}

esp_err_t audio_speed_control_init(audio_speed_control_t *ctrl, const audio_speed_control_cfg_t *cfg)
{
    esp_ae_sonic_cfg_t sonic_cfg = {0};
    esp_ae_err_t ret = ESP_AE_ERR_OK;

    ESP_RETURN_ON_FALSE(ctrl != NULL && cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->current_speed = 1.0f;
    ctrl->sample_rate = cfg->sample_rate;
    ctrl->channel = cfg->channel;
    ctrl->bits_per_sample = cfg->bits_per_sample;

    sonic_cfg.sample_rate = cfg->sample_rate;
    sonic_cfg.channel = cfg->channel;
    sonic_cfg.bits_per_sample = cfg->bits_per_sample;

    ret = esp_ae_sonic_open(&sonic_cfg, &ctrl->sonic_handle);
    ESP_RETURN_ON_FALSE(ret == ESP_AE_ERR_OK, audio_speed_control_convert_err(ret), TAG, "esp_ae_sonic_open failed");

    ret = esp_ae_sonic_set_pitch(ctrl->sonic_handle, 1.0f);
    if (ret != ESP_AE_ERR_OK) {
        esp_ae_sonic_close(ctrl->sonic_handle);
        ctrl->sonic_handle = NULL;
        return audio_speed_control_convert_err(ret);
    }
    return ESP_OK;
}

void audio_speed_control_deinit(audio_speed_control_t *ctrl)
{
    if (ctrl == NULL) {
        return;
    }
    if (ctrl->sonic_handle != NULL) {
        esp_ae_sonic_close(ctrl->sonic_handle);
        ctrl->sonic_handle = NULL;
    }
}

esp_err_t audio_speed_control_reset(audio_speed_control_t *ctrl)
{
    esp_ae_err_t ret;

    ESP_RETURN_ON_FALSE(ctrl != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    if (ctrl->sonic_handle == NULL) {
        ctrl->current_speed = 1.0f;
        return ESP_OK;
    }

    ret = esp_ae_sonic_reset(ctrl->sonic_handle);
    ESP_RETURN_ON_FALSE(ret == ESP_AE_ERR_OK, audio_speed_control_convert_err(ret), TAG, "esp_ae_sonic_reset failed");

    ret = esp_ae_sonic_set_speed(ctrl->sonic_handle, 1.0f);
    ESP_RETURN_ON_FALSE(ret == ESP_AE_ERR_OK, audio_speed_control_convert_err(ret), TAG, "esp_ae_sonic_set_speed failed");

    ret = esp_ae_sonic_set_pitch(ctrl->sonic_handle, 1.0f);
    ESP_RETURN_ON_FALSE(ret == ESP_AE_ERR_OK, audio_speed_control_convert_err(ret), TAG, "esp_ae_sonic_set_pitch failed");

    ctrl->current_speed = 1.0f;
    return ESP_OK;
}

esp_err_t audio_speed_control_apply(audio_speed_control_t *ctrl, float speed)
{
    esp_ae_err_t ret;

    ESP_RETURN_ON_FALSE(ctrl != NULL && ctrl->sonic_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    if (speed < 0.85f) {
        speed = 0.85f;
    } else if (speed > 1.15f) {
        speed = 1.15f;
    }

    if (fabsf(ctrl->current_speed - speed) < 0.01f) {
        return ESP_OK;
    }

    ret = esp_ae_sonic_set_speed(ctrl->sonic_handle, speed);
    ESP_RETURN_ON_FALSE(ret == ESP_AE_ERR_OK, audio_speed_control_convert_err(ret), TAG, "esp_ae_sonic_set_speed failed");

    ctrl->current_speed = speed;

    ESP_LOGI(TAG, "Apply sonic speed: %.2fx", speed);
    return ESP_OK;
}

float audio_speed_control_get(const audio_speed_control_t *ctrl)
{
    return ctrl->current_speed;
}

esp_err_t audio_speed_control_process(audio_speed_control_t *ctrl, const void *input, size_t input_size,
                                      void *output, size_t output_size, size_t *produced_size)
{
    uint32_t bytes_per_sample = 0;
    uint32_t remaining_in_num = 0;
    uint32_t remaining_out_num = 0;
    uint32_t total_out_num = 0;
    const uint8_t *input_ptr = NULL;
    uint8_t *output_ptr = NULL;
    esp_ae_err_t ret = ESP_AE_ERR_OK;

    ESP_RETURN_ON_FALSE(ctrl != NULL && ctrl->sonic_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid control");
    ESP_RETURN_ON_FALSE(input != NULL && output != NULL && produced_size != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid buffer");

    bytes_per_sample = audio_speed_control_bytes_per_sample(ctrl);
    ESP_RETURN_ON_FALSE(bytes_per_sample > 0, ESP_ERR_INVALID_ARG, TAG, "invalid audio format");
    ESP_RETURN_ON_FALSE((input_size % bytes_per_sample) == 0, ESP_ERR_INVALID_SIZE, TAG, "invalid input size");
    ESP_RETURN_ON_FALSE((output_size % bytes_per_sample) == 0, ESP_ERR_INVALID_SIZE, TAG, "invalid output size");

    input_ptr = (const uint8_t *)input;
    output_ptr = (uint8_t *)output;
    remaining_in_num = input_size / bytes_per_sample;
    remaining_out_num = output_size / bytes_per_sample;
    *produced_size = 0;

    while (remaining_out_num > 0) {
        esp_ae_sonic_in_data_t in_data = {
            .samples = (void *)input_ptr,
            .num = remaining_in_num,
            .consume_num = 0,
        };
        esp_ae_sonic_out_data_t out_data = {
            .samples = output_ptr,
            .needed_num = remaining_out_num,
            .out_num = 0,
        };

        ret = esp_ae_sonic_process(ctrl->sonic_handle, &in_data, &out_data);
        ESP_RETURN_ON_FALSE(ret == ESP_AE_ERR_OK, audio_speed_control_convert_err(ret), TAG, "esp_ae_sonic_process failed");

        total_out_num += out_data.out_num;
        remaining_out_num -= out_data.out_num;
        output_ptr += out_data.out_num * bytes_per_sample;
        input_ptr += in_data.consume_num * bytes_per_sample;
        remaining_in_num -= in_data.consume_num;

        if (remaining_in_num == 0 && out_data.out_num == 0) {
            break;
        }

        if (in_data.consume_num == 0 && out_data.out_num == 0) {
            break;
        }
    }

    *produced_size = total_out_num * bytes_per_sample;
    return ESP_OK;
}
