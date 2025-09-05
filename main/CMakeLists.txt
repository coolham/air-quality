idf_component_register(SRCS "winsen_sensor.c" "main.c" "lvgl_screen_ui.c" "dart_sensor.c"  "winsen_sensor.c" "wifi_station.c"
                          "protocols/mqtt_device.c"
                        PRIV_REQUIRES esp_wifi nvs_flash app_update esp_http_client esp_https_ota esp_event mqtt
                       INCLUDE_DIRS ".")
