idf_component_register(
    SRCS "app_main.c"
    REQUIRES console esp_codec_dev esp32_s3_korvo_2 esp_elastic_pcm_buffer esp_psram
    INCLUDE_DIRS "."
)
