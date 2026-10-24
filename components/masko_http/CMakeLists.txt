idf_component_register(SRCS "masko_http.c"
                       INCLUDE_DIRS "include"
                       REQUIRES masko_core masko_player esp_http_client mbedtls
                                freertos heap esp_timer)
