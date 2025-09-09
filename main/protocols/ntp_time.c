#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "protocols/mqtt_device.h"
#include "esp_sntp.h"
#include "protocols/mqtt_device.h"


// NTP时间同步并设置mqtt时间offset
void obtain_time_and_set_offset(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // 等待同步
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    if (timeinfo.tm_year >= (2016 - 1900)) {
        mqtt_device_set_time_offset(now);
        ESP_LOGI("main", "NTP time sync success, now: %ld", now);
    } else {
        ESP_LOGW("main", "NTP time sync failed");
    }
}