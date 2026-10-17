idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES masko_player masko_display masko_codec masko_core
                                waveshare__esp32_s3_touch_amoled_1_75 esp_partition)
