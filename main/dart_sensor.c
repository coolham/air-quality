#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "dart_sensor.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <time.h>

#define DART_UART_PORT_NUM      UART_NUM_1
#define DART_UART_BAUD_RATE    9600
#define DART_UART_TX_PIN       18
#define DART_UART_RX_PIN       19
#define DART_UART_BUF_SIZE     128

static const char *DART_TAG = "dart_sensor";

static QueueHandle_t dart_sensor_queue = NULL;

// Dart协议相关命令
static const uint8_t dart_cmd_switch_to_qna[9] = {0xFF, 0x01, 0x78, 0x41, 0x00, 0x00, 0x00, 0x00, 0x46};
static const uint8_t dart_cmd_read_gas[9] = {0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};

// 校验和计算
static uint8_t dart_checksum(const uint8_t *buf, uint8_t len)
{
    uint8_t sum = 0;
    for (int i = 1; i < len - 1; ++i) {
        sum += buf[i];
    }
    return (~sum) + 1;
}

void dart_sensor_init(void)
{
    ESP_LOGI(DART_TAG, "Initializing UART for Dart sensor...");
    const uart_config_t uart_config = {
        .baud_rate = DART_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_driver_install(DART_UART_PORT_NUM, DART_UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(DART_UART_PORT_NUM, &uart_config);
    uart_set_pin(DART_UART_PORT_NUM, DART_UART_TX_PIN, DART_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(DART_TAG, "Dart sensor UART initialized");
}


void dart_sensor_read(dart_sensor_data_t *data)
{
    ESP_LOGI(DART_TAG, "Sending read command to Dart sensor");
    uart_flush(DART_UART_PORT_NUM);
    uart_write_bytes(DART_UART_PORT_NUM, (const char*)dart_cmd_read_gas, sizeof(dart_cmd_read_gas));
    uint8_t rx_buf[16] = {0};
    int len = uart_read_bytes(DART_UART_PORT_NUM, rx_buf, 9, pdMS_TO_TICKS(200));
    ESP_LOGI(DART_TAG, "Received %d bytes from sensor", len);
    if (len == 9 && rx_buf[0] == 0xFF) {
        uint8_t checksum = dart_checksum(rx_buf, 9);
        ESP_LOGI(DART_TAG, "Frame: %02X %02X %02X %02X %02X %02X %02X %02X %02X", rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7], rx_buf[8]);
        if (checksum == rx_buf[8]) {
            // 解析气体浓度
            uint16_t ch2o_ugm3 = rx_buf[2] * 256 + rx_buf[3];
            uint16_t ch2o_ppb  = rx_buf[6] * 256 + rx_buf[7];
            data->ch2o_ugm3 = (float)ch2o_ugm3;
            data->ch2o_ppb  = (float)ch2o_ppb;
            data->timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            ESP_LOGI(DART_TAG, "CH2O: %u ug/m3, %u ppb, timestamp: %u", ch2o_ugm3, ch2o_ppb, data->timestamp);
        } else {
            ESP_LOGW(DART_TAG, "Checksum error: received %02X, calculated %02X", rx_buf[8], checksum);
        }
    } else {
        ESP_LOGW(DART_TAG, "UART read failed or invalid frame");
    }
}

static void dart_sensor_task(void *pvParameters) {
    ESP_LOGI(DART_TAG, "Switching Dart sensor to Q&A mode");
    uart_write_bytes(DART_UART_PORT_NUM, (const char*)dart_cmd_switch_to_qna, sizeof(dart_cmd_switch_to_qna));
    vTaskDelay(pdMS_TO_TICKS(100)); // 等待传感器切换
    dart_sensor_data_t data;
    while (1) {
        dart_sensor_read(&data);
        xQueueSend(dart_sensor_queue, &data, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1000)); // 1秒读取一次
    }
}

static void dart_sensor_print_task(void *pvParameters) {
    dart_sensor_data_t data;
    while (1) {
        if (xQueueReceive(dart_sensor_queue, &data, portMAX_DELAY) == pdTRUE) {
            float ch2o_mg = data.ch2o_ugm3 * 0.001f; // ug/m3 转 mg/m3
            time_t now = data.timestamp;
            struct tm t;
            localtime_r(&now, &t);
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &t);
            printf("Dart CH2O: %.3f mg/m3, %.1f ug/m3, %.1f ppb, time: %s\n", ch2o_mg, data.ch2o_ugm3, data.ch2o_ppb, time_str);
        }
    }
}

void dart_sensor_start(void)
{
    dart_sensor_init();
    if (!dart_sensor_queue) {
        dart_sensor_queue = xQueueCreate(8, sizeof(dart_sensor_data_t));
    }
    xTaskCreate(dart_sensor_task, "dart_sensor_task", 2048, NULL, 5, NULL);
    xTaskCreate(dart_sensor_print_task, "dart_sensor_print_task", 2048, NULL, 4, NULL);
}