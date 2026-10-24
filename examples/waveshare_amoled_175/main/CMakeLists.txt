idf_component_register(SRCS "main.c" "stream_demo.c"
                       INCLUDE_DIRS "."
                       REQUIRES masko_player masko_display masko_codec masko_core
                                masko_store masko_graph masko_http
                                waveshare__esp32_s3_touch_amoled_1_75 esp_partition
                                esp_driver_gpio esp_wifi esp_netif esp_event nvs_flash
                       EMBED_TXTFILES "kylo-stream-mascot.json")
