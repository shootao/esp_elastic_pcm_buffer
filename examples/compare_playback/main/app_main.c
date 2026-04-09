/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_console.h"
#include "esp_log.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#include "esp_random.h"
#include "sdmmc_cmd.h"

#include "bsp/esp32_s3_korvo_2.h"

#include "elastic_pcm_buffer.h"

static const char *TAG = "elastic_pcm";
static const char *EXAMPLE_NAME = "compare_playback";

#define PCM_SAMPLE_RATE             16000
#define PCM_CHANNELS                1
#define PCM_BITS_PER_SAMPLE         16
#define PCM_CHUNK_MS                60
#define PCM_FRAME_SAMPLES           ((PCM_SAMPLE_RATE * PCM_CHUNK_MS) / 1000)
#define PCM_FRAME_BYTES             (PCM_FRAME_SAMPLES * PCM_CHANNELS * (PCM_BITS_PER_SAMPLE / 8))
#define PCM_DEFAULT_FILE_PATH       BSP_SD_MOUNT_POINT "/feeder.pcm"

#define NET_JITTER_DOWN_MS_MAX      60
#define NET_JITTER_UP_MS_MAX        60

#define PLAYER_VOLUME_PERCENT       90

#define JB_CAPACITY_FRAMES          16
#define JB_START_WATERMARK          6
#define JB_LOW_WATERMARK            4
#define JB_TARGET_WATERMARK         8
#define JB_HIGH_WATERMARK           12
#define JB_SPEED_MIN                0.82f
#define JB_SPEED_NORMAL             1.00f
#define JB_SPEED_MAX                1.18f

#define PLAY_TASK_STACK_SIZE        8192 * 2
#define PRODUCER_TASK_STACK_SIZE    6144

typedef enum {
    PLAYBACK_MODE_DIRECT = 0,
    PLAYBACK_MODE_ELASTIC,
} playback_mode_t;

typedef enum {
    STOP_ACTION_NONE = 0,
    STOP_ACTION_DISCARD,
    STOP_ACTION_DRAIN_DIRECT,
} stop_action_t;

typedef struct {
    SemaphoreHandle_t lock;
    esp_codec_dev_handle_t speaker;
    TaskHandle_t play_task;
    TaskHandle_t producer_task;
    bool play_task_uses_caps;
    bool producer_task_uses_caps;
    playback_mode_t mode;
    stop_action_t stop_action;
    bool running;
    bool stop_requested;
    bool producer_done;
    char file_path[256];
    elastic_pcm_buffer_t *buffer;
    uint32_t chunks_read;
    uint32_t chunks_played;
    uint32_t chunks_dropped;
    uint32_t last_sequence;
    size_t bytes_read;
    size_t bytes_played;
} demo_ctx_t;

static demo_ctx_t s_demo = {
    .mode = PLAYBACK_MODE_DIRECT,
};
static esp_console_repl_t *s_repl = NULL;

static const char *playback_mode_to_string(playback_mode_t mode)
{
    return (mode == PLAYBACK_MODE_ELASTIC) ? "elastic" : "direct";
}

static const char *stop_action_to_string(stop_action_t action)
{
    switch (action) {
        case STOP_ACTION_DISCARD:
            return "discard";
        case STOP_ACTION_DRAIN_DIRECT:
            return "drain";
        case STOP_ACTION_NONE:
        default:
            return "none";
    }
}

static bool demo_psram_available(void)
{
#if CONFIG_SPIRAM
    return esp_psram_is_initialized();
#else
    return false;
#endif
}

static BaseType_t demo_create_task(TaskFunction_t main_func,
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
    if (stack_in_ext && demo_psram_available()) {
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
    if (stack_in_ext && demo_psram_available()) {
        ESP_LOGW(TAG, "Make sure selected the `CONFIG_SPIRAM_BOOT_INIT` and `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` by `make menuconfig`");
    }
#endif

    return xTaskCreatePinnedToCore(main_func, name, stack, arg, prio, task_handle, core_id);
}

static void demo_delete_current_task(bool task_uses_caps)
{
    if (task_uses_caps) {
        vTaskDeleteWithCaps(NULL);
        return;
    }
    vTaskDelete(NULL);
}

static uint32_t get_simulated_network_delay_ms(void)
{
    const int32_t down = NET_JITTER_DOWN_MS_MAX;
    const int32_t up = NET_JITTER_UP_MS_MAX;
    const int32_t span = down + up + 1;
    int32_t offset = 0;
    int32_t delay = PCM_CHUNK_MS;

    if (span > 1) {
        offset = (int32_t)(esp_random() % (uint32_t)span) - down;
    }
    delay += offset;
    if (delay < 1) {
        delay = 1;
    }
    return (uint32_t)delay;
}

static bool demo_is_stop_requested(void)
{
    bool stop = false;

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    stop = s_demo.stop_requested;
    xSemaphoreGive(s_demo.lock);
    return stop;
}

static stop_action_t demo_get_stop_action(void)
{
    stop_action_t action;

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    action = s_demo.stop_action;
    xSemaphoreGive(s_demo.lock);
    return action;
}

static void demo_reset_stats_locked(void)
{
    s_demo.stop_action = STOP_ACTION_NONE;
    s_demo.producer_done = false;
    s_demo.play_task_uses_caps = false;
    s_demo.producer_task_uses_caps = false;
    s_demo.chunks_read = 0;
    s_demo.chunks_played = 0;
    s_demo.chunks_dropped = 0;
    s_demo.last_sequence = 0;
    s_demo.bytes_read = 0;
    s_demo.bytes_played = 0;
}

static esp_err_t demo_write_to_speaker(void *data, size_t size)
{
    int ret = esp_codec_dev_write(s_demo.speaker, data, (int)size);
    ESP_RETURN_ON_FALSE(ret == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "speaker write failed");

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    s_demo.chunks_played++;
    s_demo.bytes_played += size;
    xSemaphoreGive(s_demo.lock);
    return ESP_OK;
}

static void demo_cleanup_playback(void)
{
    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    s_demo.stop_action = STOP_ACTION_NONE;
    s_demo.stop_requested = false;
    s_demo.producer_done = false;
    s_demo.running = false;
    s_demo.play_task = NULL;
    s_demo.producer_task = NULL;
    s_demo.play_task_uses_caps = false;
    s_demo.producer_task_uses_caps = false;
    if (s_demo.buffer != NULL) {
        elastic_pcm_buffer_destroy(s_demo.buffer);
        s_demo.buffer = NULL;
    }
    xSemaphoreGive(s_demo.lock);
}

static void log_status_snapshot(void)
{
    playback_mode_t mode;
    bool running;
    bool stop_requested;
    stop_action_t stop_action;
    bool producer_done;
    uint32_t chunks_read;
    uint32_t chunks_played;
    uint32_t chunks_dropped;
    size_t bytes_read;
    size_t bytes_played;
    char file_path[256];

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    mode = s_demo.mode;
    running = s_demo.running;
    stop_requested = s_demo.stop_requested;
    stop_action = s_demo.stop_action;
    producer_done = s_demo.producer_done;
    chunks_read = s_demo.chunks_read;
    chunks_played = s_demo.chunks_played;
    chunks_dropped = s_demo.chunks_dropped;
    bytes_read = s_demo.bytes_read;
    bytes_played = s_demo.bytes_played;
    strlcpy(file_path, s_demo.file_path, sizeof(file_path));
    xSemaphoreGive(s_demo.lock);

    ESP_LOGI(TAG,
             "mode=%s running=%s stop=%s stop_action=%s producer_done=%s file=%s read_chunks=%" PRIu32 " played_chunks=%" PRIu32 " dropped=%" PRIu32 " read_bytes=%u played_bytes=%u",
             playback_mode_to_string(mode),
             running ? "yes" : "no",
             stop_requested ? "yes" : "no",
             stop_action_to_string(stop_action),
             producer_done ? "yes" : "no",
             file_path[0] ? file_path : "(none)",
             chunks_read,
             chunks_played,
             chunks_dropped,
             (unsigned)bytes_read,
             (unsigned)bytes_played);

    if (mode == PLAYBACK_MODE_ELASTIC && s_demo.buffer != NULL) {
        elastic_pcm_buffer_status_t status = elastic_pcm_buffer_get_status(s_demo.buffer);
        ESP_LOGI(TAG,
                 "elastic level=%u/%u state=%s prefill=%s speed=%.2fx underflow=%" PRIu32 " overflow=%" PRIu32,
                 (unsigned)status.level,
                 (unsigned)status.capacity,
                 elastic_pcm_buffer_state_to_string(status.state),
                 status.prefill_mode ? "yes" : "no",
                 status.recommended_speed,
                 status.underflow_count,
                 status.overflow_count);
    }
}

static void elastic_producer_task(void *arg)
{
    FILE *fp = NULL;
    uint32_t sequence = 0;
    uint8_t frame[PCM_FRAME_BYTES];
    char file_path[256];
    bool task_uses_caps;

    (void)arg;

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    strlcpy(file_path, s_demo.file_path, sizeof(file_path));
    task_uses_caps = s_demo.producer_task_uses_caps;
    xSemaphoreGive(s_demo.lock);

    fp = fopen(file_path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open PCM file for producer: %s", file_path);
        xSemaphoreTake(s_demo.lock, portMAX_DELAY);
        s_demo.producer_done = true;
        s_demo.producer_task = NULL;
        s_demo.producer_task_uses_caps = false;
        xSemaphoreGive(s_demo.lock);
        demo_delete_current_task(task_uses_caps);
        return;
    }

    while (!demo_is_stop_requested()) {
        size_t bytes_read = fread(frame, 1, sizeof(frame), fp);
        uint32_t delay_ms;
        esp_err_t err;

        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < sizeof(frame)) {
            memset(frame + bytes_read, 0, sizeof(frame) - bytes_read);
        }

        delay_ms = get_simulated_network_delay_ms();
        vTaskDelay(pdMS_TO_TICKS(delay_ms));

        if (demo_is_stop_requested()) {
            break;
        }

        err = elastic_pcm_buffer_push(s_demo.buffer, frame, sizeof(frame), sequence);

        xSemaphoreTake(s_demo.lock, portMAX_DELAY);
        s_demo.chunks_read++;
        s_demo.bytes_read += sizeof(frame);
        s_demo.last_sequence = sequence;
        if (err != ESP_OK) {
            s_demo.chunks_dropped++;
        }
        xSemaphoreGive(s_demo.lock);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "elastic buffer push failed seq=%" PRIu32 ": %s", sequence, esp_err_to_name(err));
        }
        sequence++;
    }

    fclose(fp);

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    s_demo.producer_done = true;
    s_demo.producer_task = NULL;
    s_demo.producer_task_uses_caps = false;
    xSemaphoreGive(s_demo.lock);

    demo_delete_current_task(task_uses_caps);
}

static esp_err_t elastic_output_to_speaker_cb(void *ctx, const void *data, size_t size,
                                              const elastic_pcm_buffer_frame_meta_t *meta)
{
    (void)ctx;
    (void)meta;
    return demo_write_to_speaker((void *)data, size);
}

static esp_err_t run_direct_playback(const char *file_path)
{
    FILE *fp = fopen(file_path, "rb");
    uint8_t frame[PCM_FRAME_BYTES];

    ESP_RETURN_ON_FALSE(fp != NULL, ESP_ERR_NOT_FOUND, TAG, "failed to open PCM file");

    while (!demo_is_stop_requested()) {
        size_t bytes_read = fread(frame, 1, sizeof(frame), fp);
        uint32_t delay_ms;

        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < sizeof(frame)) {
            memset(frame + bytes_read, 0, sizeof(frame) - bytes_read);
        }

        delay_ms = get_simulated_network_delay_ms();
        vTaskDelay(pdMS_TO_TICKS(delay_ms));

        if (demo_is_stop_requested()) {
            break;
        }

        xSemaphoreTake(s_demo.lock, portMAX_DELAY);
        s_demo.chunks_read++;
        s_demo.bytes_read += sizeof(frame);
        xSemaphoreGive(s_demo.lock);

        ESP_RETURN_ON_ERROR(demo_write_to_speaker(frame, sizeof(frame)), TAG, "direct playback failed");
    }

    fclose(fp);
    return ESP_OK;
}

static esp_err_t run_elastic_playback(const char *file_path)
{
    elastic_pcm_buffer_cfg_t buffer_cfg = {
        .capacity = JB_CAPACITY_FRAMES,
        .frame_size = PCM_FRAME_BYTES,
        .start_watermark = JB_START_WATERMARK,
        .low_watermark = JB_LOW_WATERMARK,
        .target_watermark = JB_TARGET_WATERMARK,
        .high_watermark = JB_HIGH_WATERMARK,
        .min_speed = JB_SPEED_MIN,
        .normal_speed = JB_SPEED_NORMAL,
        .max_speed = JB_SPEED_MAX,
    };
    elastic_pcm_buffer_pipeline_cfg_t pipeline_cfg = {
        .sample_rate = PCM_SAMPLE_RATE,
        .channel = PCM_CHANNELS,
        .bits_per_sample = PCM_BITS_PER_SAMPLE,
        .consumer_task_stack_size = 6144,
        .consumer_task_priority = 5,
        .output_cb = elastic_output_to_speaker_cb,
        .output_ctx = NULL,
    };
    esp_err_t ret = ESP_OK;
    bool stop_sent = false;

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    s_demo.buffer = elastic_pcm_buffer_create(&buffer_cfg);
    xSemaphoreGive(s_demo.lock);
    ESP_RETURN_ON_FALSE(s_demo.buffer != NULL, ESP_ERR_NO_MEM, TAG, "failed to create elastic PCM buffer");

    ESP_GOTO_ON_ERROR(elastic_pcm_buffer_set_pipeline(s_demo.buffer, &pipeline_cfg), err, TAG, "failed to configure pipeline");
    ESP_GOTO_ON_ERROR(elastic_pcm_buffer_start(s_demo.buffer), err, TAG, "failed to start elastic PCM buffer");
    ESP_GOTO_ON_ERROR(elastic_pcm_buffer_session_begin(s_demo.buffer), err, TAG, "failed to begin elastic PCM buffer session");

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    s_demo.producer_done = false;
    xSemaphoreGive(s_demo.lock);

    if (demo_create_task(elastic_producer_task, "elastic_producer", PRODUCER_TASK_STACK_SIZE, NULL, 5,
                         &s_demo.producer_task, tskNO_AFFINITY, true,
                         &s_demo.producer_task_uses_caps) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "elastic mode started, reading %s", file_path);

    while (true) {
        elastic_pcm_buffer_status_t status = elastic_pcm_buffer_get_status(s_demo.buffer);
        stop_action_t stop_action = demo_get_stop_action();

        if (!stop_sent && stop_action != STOP_ACTION_NONE) {
            elastic_pcm_buffer_stop_mode_t buffer_stop_mode =
                (stop_action == STOP_ACTION_DRAIN_DIRECT) ? ELASTIC_PCM_BUFFER_STOP_MODE_DRAIN_DIRECT : ELASTIC_PCM_BUFFER_STOP_MODE_DISCARD;
            ESP_LOGI(TAG, "Forward session end request to elastic PCM buffer: %s", stop_action_to_string(stop_action));
            ESP_GOTO_ON_ERROR(elastic_pcm_buffer_session_end(s_demo.buffer, buffer_stop_mode), err, TAG,
                              "failed to end elastic PCM buffer session");
            stop_sent = true;
        }

        xSemaphoreTake(s_demo.lock, portMAX_DELAY);
        if (s_demo.producer_done && status.level == 0) {
            xSemaphoreGive(s_demo.lock);
            break;
        }
        xSemaphoreGive(s_demo.lock);

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    while (s_demo.producer_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;

err:
    while (s_demo.producer_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ret;
}

static void playback_task(void *arg)
{
    playback_mode_t mode;
    char file_path[256];
    esp_err_t ret;
    bool task_uses_caps;

    (void)arg;

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    mode = s_demo.mode;
    strlcpy(file_path, s_demo.file_path, sizeof(file_path));
    task_uses_caps = s_demo.play_task_uses_caps;
    xSemaphoreGive(s_demo.lock);

    ESP_LOGI(TAG,
             "Playback start mode=%s file=%s chunk=%dms frame=%d bytes jitter=[-%d,+%d]ms",
             playback_mode_to_string(mode),
             file_path,
             PCM_CHUNK_MS,
             PCM_FRAME_BYTES,
             NET_JITTER_DOWN_MS_MAX,
             NET_JITTER_UP_MS_MAX);

    if (mode == PLAYBACK_MODE_ELASTIC) {
        ret = run_elastic_playback(file_path);
    } else {
        ret = run_direct_playback(file_path);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Playback failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Playback finished");
    }

    log_status_snapshot();
    demo_cleanup_playback();
    demo_delete_current_task(task_uses_caps);
}

static int demo_start_playback(const char *file_path)
{
    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    if (s_demo.running) {
        xSemaphoreGive(s_demo.lock);
        return 1;
    }

    demo_reset_stats_locked();
    s_demo.running = true;
    s_demo.stop_requested = false;
    s_demo.stop_action = STOP_ACTION_NONE;
    strlcpy(s_demo.file_path, file_path ? file_path : PCM_DEFAULT_FILE_PATH, sizeof(s_demo.file_path));
    xSemaphoreGive(s_demo.lock);

    if (demo_create_task(playback_task, "playback_task", PLAY_TASK_STACK_SIZE, NULL, 5,
                         &s_demo.play_task, tskNO_AFFINITY, true,
                         &s_demo.play_task_uses_caps) != pdPASS) {
        xSemaphoreTake(s_demo.lock, portMAX_DELAY);
        s_demo.running = false;
        s_demo.play_task = NULL;
        s_demo.play_task_uses_caps = false;
        xSemaphoreGive(s_demo.lock);
        return 1;
    }
    return 0;
}

static int cmd_play(int argc, char **argv)
{
    const char *file_path = (argc >= 2) ? argv[1] : PCM_DEFAULT_FILE_PATH;

    if (demo_start_playback(file_path) != 0) {
        printf("failed to start playback, or playback is already running\n");
        return 1;
    }

    printf("[%s] start playback: mode=%s file=%s\n",
           EXAMPLE_NAME, playback_mode_to_string(s_demo.mode), s_demo.file_path);
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    stop_action_t stop_action = STOP_ACTION_DISCARD;

    if (argc >= 2) {
        if (strcmp(argv[1], "discard") == 0) {
            stop_action = STOP_ACTION_DISCARD;
        } else if (strcmp(argv[1], "drain") == 0) {
            stop_action = STOP_ACTION_DRAIN_DIRECT;
        } else {
            printf("[%s] usage: stop [discard|drain]\n", EXAMPLE_NAME);
            return 1;
        }
    }

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    if (!s_demo.running) {
        xSemaphoreGive(s_demo.lock);
        printf("[%s] playback is not running\n", EXAMPLE_NAME);
        return 0;
    }
    s_demo.stop_action = stop_action;
    s_demo.stop_requested = true;
    xSemaphoreGive(s_demo.lock);

    printf("[%s] stop requested: %s\n", EXAMPLE_NAME, stop_action_to_string(stop_action));
    return 0;
}

static int cmd_mode(int argc, char **argv)
{
    playback_mode_t mode;

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    if (argc == 1) {
        printf("[%s] current mode: %s\n", EXAMPLE_NAME, playback_mode_to_string(s_demo.mode));
        xSemaphoreGive(s_demo.lock);
        return 0;
    }

    if (s_demo.running) {
        xSemaphoreGive(s_demo.lock);
        printf("[%s] cannot change mode while playback is running\n", EXAMPLE_NAME);
        return 1;
    }

    if (strcmp(argv[1], "elastic") == 0 || strcmp(argv[1], "jb") == 0) {
        mode = PLAYBACK_MODE_ELASTIC;
    } else if (strcmp(argv[1], "direct") == 0) {
        mode = PLAYBACK_MODE_DIRECT;
    } else {
        xSemaphoreGive(s_demo.lock);
        printf("[%s] usage: mode [elastic|jb|direct]\n", EXAMPLE_NAME);
        return 1;
    }

    s_demo.mode = mode;
    xSemaphoreGive(s_demo.lock);

    printf("[%s] playback mode set to %s\n", EXAMPLE_NAME, playback_mode_to_string(mode));
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    log_status_snapshot();
    return 0;
}

static void register_console_commands(void)
{
    const esp_console_cmd_t play_cmd = {
        .command = "play",
        .help = "compare example: play PCM file from SD card, default path is " PCM_DEFAULT_FILE_PATH,
        .hint = NULL,
        .func = cmd_play,
    };
    const esp_console_cmd_t stop_cmd = {
        .command = "stop",
        .help = "stop current playback: stop [discard|drain], default is discard",
        .hint = NULL,
        .func = cmd_stop,
    };
    const esp_console_cmd_t mode_cmd = {
        .command = "mode",
        .help = "get or set playback mode: mode [elastic|direct], `jb` is kept as alias",
        .hint = NULL,
        .func = cmd_mode,
    };
    const esp_console_cmd_t status_cmd = {
        .command = "status",
        .help = "print compare example playback status and elastic PCM buffer status",
        .hint = NULL,
        .func = cmd_status,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&play_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&mode_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));
    esp_console_register_help_command();
}

static void init_console(void)
{
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &s_repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&cdc_config, &repl_config, &s_repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &s_repl));
#else
#error "No console device configured"
#endif

    register_console_commands();
    ESP_ERROR_CHECK(esp_console_start_repl(s_repl));
}

static void init_korvo2_audio_and_storage(void)
{
    const i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PCM_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = PCM_BITS_PER_SAMPLE,
        .channel = PCM_CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = PCM_SAMPLE_RATE,
        .mclk_multiple = 0,
    };

    ESP_ERROR_CHECK(bsp_sdcard_mount());
    sdmmc_card_print_info(stdout, bsp_sdcard_get_handle());

    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_audio_init(&i2s_cfg));

    s_demo.speaker = bsp_audio_codec_speaker_init();
    ESP_ERROR_CHECK(s_demo.speaker != NULL ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_codec_dev_open(s_demo.speaker, &sample_info));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_demo.speaker, PLAYER_VOLUME_PERCENT));

    ESP_LOGI(TAG, "Korvo-2 audio + SD card ready");
    ESP_LOGI(TAG, "PCM format: %d Hz, %d-bit, ch=%d, frame=%d bytes (%d ms)",
             PCM_SAMPLE_RATE, PCM_BITS_PER_SAMPLE, PCM_CHANNELS, PCM_FRAME_BYTES, PCM_CHUNK_MS);
}

void app_main(void)
{
    s_demo.lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_demo.lock != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    init_korvo2_audio_and_storage();

    ESP_LOGI(TAG, "Example: %s", EXAMPLE_NAME);
    ESP_LOGI(TAG, "This example compares direct playback and elastic PCM buffer playback.");
    ESP_LOGI(TAG, "Commands:");
    ESP_LOGI(TAG, "  mode elastic");
    ESP_LOGI(TAG, "  mode jb");
    ESP_LOGI(TAG, "  mode direct");
    ESP_LOGI(TAG, "  play " PCM_DEFAULT_FILE_PATH);
    ESP_LOGI(TAG, "  stop");
    ESP_LOGI(TAG, "  status");

    init_console();

    xSemaphoreTake(s_demo.lock, portMAX_DELAY);
    s_demo.mode = PLAYBACK_MODE_DIRECT;
    xSemaphoreGive(s_demo.lock);

    if (demo_start_playback(PCM_DEFAULT_FILE_PATH) != 0) {
        ESP_LOGE(TAG, "auto playback start failed: mode=%s file=%s",
                 playback_mode_to_string(s_demo.mode), PCM_DEFAULT_FILE_PATH);
    } else {
        ESP_LOGI(TAG, "compare demo auto playback started: mode=%s file=%s",
                 playback_mode_to_string(s_demo.mode), PCM_DEFAULT_FILE_PATH);
    }
}
