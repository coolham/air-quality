
/*
Air Quality Monitoring:

甲醛（英语：Formaldehyde），化学式HCHO

*/

#include <stdio.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "dart_sensor.h"
#include "winsen_sensor.h"
#include "freertos/queue.h"
#include "lvgl_screen_ui.h"
#include "wifi_station.h"
#include "protocols/ntp_time.h"
#include "protocols/mqtt_device.h"

#define APP_VERSION "v1.0.0"

#if CONFIG_LCD_CONTROLLER_SH1107
#include "esp_lcd_sh1107.h"
#else
#include "esp_lcd_panel_vendor.h"
#endif

extern float g_dart_hcho_mg, g_dart_hcho_ppb;
extern float g_winsen_hcho_mg, g_winsen_hcho_ppb;

static const char *TAG = "main";

#define I2C_BUS_PORT  0

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define AIR_LCD_PIXEL_CLOCK_HZ    (400 * 1000)
#define AIR_PIN_NUM_SDA           17
#define AIR_PIN_NUM_SCL           16
#define AIR_PIN_NUM_RST           -1
#define AIR_I2C_HW_ADDR           0x3C


// Bit number used to represent command and parameter
#define AIR_LCD_CMD_BITS           8
#define AIR_LCD_PARAM_BITS         8


i2c_master_bus_handle_t i2c_bus = NULL;
esp_lcd_panel_io_handle_t io_handle = NULL;
esp_lcd_panel_handle_t panel_handle = NULL;


esp_err_t init_i2c_bus()
{
    ESP_LOGI(TAG, "Initialize I2C bus");

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_BUS_PORT,
        .sda_io_num = AIR_PIN_NUM_SDA,
        .scl_io_num = AIR_PIN_NUM_SCL,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));
    return ESP_OK;
}

esp_err_t init_lcd_device()
{
    ESP_LOGI(TAG, "Install panel IO");

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = AIR_I2C_HW_ADDR,
        .scl_speed_hz = AIR_LCD_PIXEL_CLOCK_HZ,
        .control_phase_bytes = 1,               // According to SSD1306 datasheet
        .lcd_cmd_bits = AIR_LCD_CMD_BITS,   // According to SSD1306 datasheet
        .lcd_param_bits = AIR_LCD_CMD_BITS, // According to SSD1306 datasheet
#if CONFIG_LCD_CONTROLLER_SSD1306
        .dc_bit_offset = 6,                     // According to SSD1306 datasheet
#elif CONFIG_LCD_CONTROLLER_SH1107
        .dc_bit_offset = 0,                     // According to SH1107 datasheet
        .flags =
        {
            .disable_control_phase = 1,
        }
#endif
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install SSD1306 panel driver");
  
  
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = AIR_PIN_NUM_RST,
    };
#if CONFIG_LCD_CONTROLLER_SSD1306
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = AIR_LCD_V_RES,
    };
    panel_config.vendor_config = &ssd1306_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle));
#elif CONFIG_LCD_CONTROLLER_SH1107
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh1107(io_handle, &panel_config, &panel_handle));
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

#if CONFIG_LCD_CONTROLLER_SH1107
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
#endif

    return ESP_OK;
}

void air_quality_mqtt_task(void *arg) {
    // air_quality_data_t data;
    while (1) {
        // data.dart_hcho_mg = g_dart_hcho_mg;
        // data.dart_hcho_ppb = g_dart_hcho_ppb;
        // data.winsen_hcho_mg = g_winsen_hcho_mg;
        // data.winsen_hcho_ppb = g_winsen_hcho_ppb;
        // mqtt_device_publish_air_quality(&data);
        vTaskDelay(pdMS_TO_TICKS(5000)); // 每5秒上传一次
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Air Quality Monitor - Version: %s", APP_VERSION);

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化I2C总线
    init_i2c_bus();


    // Initialize LCD device
    init_lcd_device();

    init_lvgl_display();

    // 启动 Dart 传感器功能（队列、任务、打印）
    dart_sensor_start();
    winsen_sensor_start();


    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
        /* If you only want to open more logs in the wifi module, you need to make the max level greater than the default level,
         * and call esp_log_level_set() before esp_wifi_init() to improve the log level of the wifi module. */
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
    }

    // init WiFi

    wifi_init_sta();


    // 只有WiFi连接成功才同步NTP并启动MQTT
    if (wifi_get_event_bits() & BIT0) {
        obtain_time_and_set_offset();
        ESP_LOGI(TAG, "WiFi connected, starting MQTT client.");
        mqtt_device_start();
    } else {
        ESP_LOGE(TAG, "WiFi not connected, MQTT client will not start.");
    }

    // xTaskCreate(air_quality_mqtt_task, "air_quality_mqtt_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "main init completed.");

    while(1){
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
