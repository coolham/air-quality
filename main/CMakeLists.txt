idf_component_register(SRCS "main.c" "lvgl_screen_ui.c" "dart_sensor.c" "wifi_station.c"
                        PRIV_REQUIRES esp_wifi nvs_flash app_update esp_http_client esp_https_ota
                       INCLUDE_DIRS ".")
