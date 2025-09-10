idf_component_register(SRCS "sensor_filter.c" "winsen_sensor.c""lvgl_screen_ui.c" "dart_sensor.c"  "winsen_sensor.c" "wifi_station.c"
                          "protocols/mqtt_device.c" "protocols/ntp_time.c"
                           "main.c" 
                        PRIV_REQUIRES esp_wifi nvs_flash app_update esp_http_client esp_https_ota esp_event mqtt
                       INCLUDE_DIRS ".")
