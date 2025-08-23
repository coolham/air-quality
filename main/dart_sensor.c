#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "dart_sensor.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <time.h>
#include "lvgl_screen_ui.h"
#include "sys/lock.h"

#define DART_UART_PORT_NUM      UART_NUM_1
#define DART_UART_BAUD_RATE    9600
#define DART_UART_TX_PIN       18
#define DART_UART_RX_PIN       19
#define DART_UART_BUF_SIZE     128

static const char *TAG = "dart_sensor";

static QueueHandle_t dart_sensor_queue = NULL;
_lock_t lvgl_api_lock;

float g_ch2o_mg = 0.0f;
uint32_t g_ch2o_timestamp = 0;
static uint32_t g_dart_read_count = 0;

// Dart协议相关命令
static const uint8_t dart_cmd_switch_to_qna[9] = {0xFF, 0x01, 0x78, 0x41, 0x00, 0x00, 0x00, 0x00, 0x46};
static const uint8_t dart_cmd_switch_to_auto[9] = {0xFF, 0x01, 0x78, 0x40, 0x00, 0x00, 0x00, 0x00, 0x47};
static const uint8_t dart_cmd_read_gas[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};

typedef enum {
    DART_SENSOR_MODE_AUTO = 0, // 自动上传模式
    DART_SENSOR_MODE_QNA  = 1  // 问答模式
} dart_sensor_mode_t;

static dart_sensor_mode_t g_dart_sensor_mode = DART_SENSOR_MODE_AUTO;

// 用于存储UART接收数据的静态缓冲区
static uint8_t g_rx_buf[64] = {0};  // 增大缓冲区以容纳更多数据
static int g_rx_buf_pos = 0;  // 当前缓冲区位置，用于追加新数据

// 初始化传感器工作模式
static void dart_sensor_init_mode(void)
{
    if (g_dart_sensor_mode == DART_SENSOR_MODE_QNA) {
        ESP_LOGI(TAG, "Switching Dart sensor to Q&A mode");
        uart_write_bytes(DART_UART_PORT_NUM, (const char*)dart_cmd_switch_to_qna, sizeof(dart_cmd_switch_to_qna));
        vTaskDelay(pdMS_TO_TICKS(1000)); // 等待传感器切换
    } else {
        ESP_LOGI(TAG, "Switching Dart sensor to AUTO mode");
        uart_write_bytes(DART_UART_PORT_NUM, (const char*)dart_cmd_switch_to_auto, sizeof(dart_cmd_switch_to_auto));
        vTaskDelay(pdMS_TO_TICKS(1000)); // 等待传感器切换
    }
};


void dart_sensor_init(void)
{
    ESP_LOGI(TAG, "Initializing UART for Dart sensor...");
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
    ESP_LOGI(TAG, "Dart sensor UART initialized");

}

// 设置数据无效
static void set_data_invalid(dart_sensor_data_t *data)
{
    data->ch2o_ugm3 = 0.0f;
    data->ch2o_ppb = 0.0f;
    data->timestamp = 0;
    data->count = 0;
}

// 校验和计算
static uint8_t dart_checksum(const uint8_t *buf, uint8_t len)
{
    uint8_t sum = 0;
    for (int i = 1; i < len - 1; ++i) {
        sum += buf[i];
    }
    return (~sum) + 1;
}

// 从UART读取原始数据
static int dart_sensor_read_raw(void)
{
    // 在问答模式下，需要先发送读取命令
    if (g_dart_sensor_mode == DART_SENSOR_MODE_QNA) {
        ESP_LOGI(TAG, "Sending read command to Dart sensor (Q&A mode)");
        // 确保缓冲区是空的，避免读取到旧数据
        uart_flush_input(DART_UART_PORT_NUM);
        // 发送气体浓度读取命令
        uart_write_bytes(DART_UART_PORT_NUM, (const char*)dart_cmd_read_gas, sizeof(dart_cmd_read_gas));
        // 简化日志输出，减少栈使用
        ESP_LOGI(TAG, "Sent gas read command");
        
        // 在QNA模式下每次发送命令后重置缓冲区，因为我们期望得到一个完整的新响应
        g_rx_buf_pos = 0;
        memset(g_rx_buf, 0, sizeof(g_rx_buf));
    } else {
        // 主动上传模式下，直接等待传感器发送数据
        ESP_LOGI(TAG, "Waiting for auto data, buffer pos: %d", g_rx_buf_pos);
        
        // 在AUTO模式下，我们保留之前的数据，可能包含部分帧
        // 如果缓冲区已经快满了，则保留末尾至少9字节（可能的完整帧），丢弃更早的数据
        if (g_rx_buf_pos > sizeof(g_rx_buf) - 18) { // 只留18字节的空间就需要清理
            ESP_LOGW(TAG, "Buffer nearly full (%d bytes), preserving only recent data", g_rx_buf_pos);
            
            // 保留最后18字节（两个可能的完整帧），丢弃更早的数据
            int bytes_to_keep = (g_rx_buf_pos >= 18) ? 18 : g_rx_buf_pos;
            memmove(g_rx_buf, g_rx_buf + g_rx_buf_pos - bytes_to_keep, bytes_to_keep);
            g_rx_buf_pos = bytes_to_keep;
            // 清空移动后的未使用部分
            memset(g_rx_buf + g_rx_buf_pos, 0, sizeof(g_rx_buf) - g_rx_buf_pos);
        }
    }
    
    // 记录当前缓冲区位置，用于计算新接收的数据长度
    int start_pos = g_rx_buf_pos;
    
    // 在问答模式下，等待时间可以短一些，因为发送命令后立即会有响应
    // 在自动上传模式下，需要等待更长时间，因为传感器每秒才发送一次数据
    int timeout = (g_dart_sensor_mode == DART_SENSOR_MODE_QNA) ? 15 : 20; // 问答模式等待150ms，自动模式等待200ms
    
    while (g_rx_buf_pos < sizeof(g_rx_buf) && timeout-- > 0) {
        int len = uart_read_bytes(DART_UART_PORT_NUM, g_rx_buf + g_rx_buf_pos, 
                                 sizeof(g_rx_buf) - g_rx_buf_pos, pdMS_TO_TICKS(10));
        if (len > 0) {
            g_rx_buf_pos += len;
            ESP_LOGD(TAG, "Received %d bytes, now has %d bytes", len, g_rx_buf_pos);
        }
        
        // 在QNA模式下，如果已经接收到足够数据（至少9字节）可以提前结束
        if (g_dart_sensor_mode == DART_SENSOR_MODE_QNA && (g_rx_buf_pos - start_pos) >= 9) {
            ESP_LOGI(TAG, "Q&A mode: Got response data, stopping");
            break;
        }
    }
    
    int new_bytes = g_rx_buf_pos - start_pos;
    
    // 如果接收了很多数据但缓冲区接近已满，可能需要立即处理
    if (new_bytes > 0 && g_rx_buf_pos > sizeof(g_rx_buf) - 9) {
        ESP_LOGW(TAG, "Buffer nearly full after reading (%d bytes), immediate processing needed", g_rx_buf_pos);
    }
    
    ESP_LOGI(TAG, "Received %d new bytes, total: %d bytes", new_bytes, g_rx_buf_pos);
    
    // 打印实际接收到的原始数据（只打印新接收的部分，限制长度）
    if (new_bytes > 0 && start_pos < g_rx_buf_pos) {
        // 减少输出长度，只输出最多9个字节，避免栈溢出
        ESP_LOGI(TAG, "Raw UART data: New data: %02X %02X %02X %02X %02X %02X %02X %02X %02X %s",
                 g_rx_buf[start_pos], 
                 (start_pos+1 < g_rx_buf_pos) ? g_rx_buf[start_pos+1] : 0,
                 (start_pos+2 < g_rx_buf_pos) ? g_rx_buf[start_pos+2] : 0,
                 (start_pos+3 < g_rx_buf_pos) ? g_rx_buf[start_pos+3] : 0,
                 (start_pos+4 < g_rx_buf_pos) ? g_rx_buf[start_pos+4] : 0,
                 (start_pos+5 < g_rx_buf_pos) ? g_rx_buf[start_pos+5] : 0,
                 (start_pos+6 < g_rx_buf_pos) ? g_rx_buf[start_pos+6] : 0,
                 (start_pos+7 < g_rx_buf_pos) ? g_rx_buf[start_pos+7] : 0,
                 (start_pos+8 < g_rx_buf_pos) ? g_rx_buf[start_pos+8] : 0,
                 (new_bytes > 8) ? "..." : "");
    }
    
    return g_rx_buf_pos;  // 返回缓冲区中的总数据量
}

// 处理接收到的数据帧
static bool dart_sensor_process_frame(const uint8_t *frame, dart_sensor_data_t *data)
{
    uint8_t checksum = dart_checksum(frame, 9);
    if (checksum != frame[8]) {
        ESP_LOGW(TAG, "Checksum error: %02X != %02X", frame[8], checksum);
        set_data_invalid(data);
        return false;
    }
    
    // 简化日志，减少栈使用
    ESP_LOGD(TAG, "Frame: %02X %02X %02X %02X...", frame[0], frame[1], frame[2], frame[3]);
    
    // 处理不同的帧类型
    if (g_dart_sensor_mode == DART_SENSOR_MODE_QNA) {
        // 问答模式：期望收到的是读取气体浓度的响应帧 (0x86)
        if (frame[1] == 0x86) {
            // 读取气体浓度帧 (response to 读取气体浓度)
            uint16_t ch2o_ugm3 = frame[2] * 256 + frame[3];  // ug/m3
            uint16_t ch2o_ppb  = frame[6] * 256 + frame[7];  // ppb
            data->ch2o_ugm3 = (float)ch2o_ugm3;
            data->ch2o_ppb  = (float)ch2o_ppb;
            data->timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            g_dart_read_count++;
            data->count = g_dart_read_count;
            ESP_LOGI(TAG, "CH2O (Q&A): %u ug/m3, %u ppb", ch2o_ugm3, ch2o_ppb);
            return true;
        } else {
            ESP_LOGW(TAG, "Q&A: Unexpected frame: 0x%02X", frame[1]);
        }
    } else {
        // 自动上传模式：期望收到的是主动上传的数据帧 (0x17)
        if (frame[1] == 0x17) {
            // 主动上传的数据帧处理
            // 根据协议，气体浓度在位置4和5
            uint16_t gas_value = frame[4] * 256 + frame[5];  // 气体浓度
            uint16_t full_scale = frame[6] * 256 + frame[7]; // 满量程
            
            // 单位在位置2，0x04表示ppb
            bool is_ppb = (frame[2] == 0x04);
            
            // 计算实际值
            if (is_ppb) {
                data->ch2o_ppb = (float)gas_value;
                data->ch2o_ugm3 = data->ch2o_ppb * 1.23f; // 转换公式：1ppb约等于1.23ug/m3
            } else {
                data->ch2o_ugm3 = (float)gas_value;
                data->ch2o_ppb = data->ch2o_ugm3 / 1.23f;
            }
            
            data->timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            g_dart_read_count++;
            data->count = g_dart_read_count;

            ESP_LOGI(TAG, "CH2O (AUTO): %.1f ug/m3, %.1f ppb, raw_data: %u, full_scale: %u", data->ch2o_ugm3, data->ch2o_ppb, gas_value, full_scale);
            return true;
        } else if (frame[1] == 0x86) {
            // 也可能收到0x86帧，尤其是在切换模式时
            uint16_t ch2o_ugm3 = frame[2] * 256 + frame[3];
            uint16_t ch2o_ppb  = frame[6] * 256 + frame[7];
            data->ch2o_ugm3 = (float)ch2o_ugm3;
            data->ch2o_ppb  = (float)ch2o_ppb;
            data->timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            g_dart_read_count++;
            data->count = g_dart_read_count;
            ESP_LOGI(TAG, "CH2O (0x86/AUTO): %u ug/m3, %u ppb", ch2o_ugm3, ch2o_ppb);
            return true;
        }
    }
    
    ESP_LOGW(TAG, "Unhandled frame type: 0x%02X", frame[1]);
    set_data_invalid(data);
    return false;
}

// 读取并处理传感器数据
static bool dart_sensor_read(dart_sensor_data_t *data)
{
    // 首先清空数据
    set_data_invalid(data);
    
    // 从传感器读取原始数据
    // 在Q&A模式下，dart_sensor_read_raw会发送读取命令，然后等待响应
    // 在AUTO模式下，dart_sensor_read_raw只会等待传感器自动发送的数据
    int total = dart_sensor_read_raw();
    if (total < 9) {
        if (g_dart_sensor_mode == DART_SENSOR_MODE_QNA) {
            ESP_LOGW(TAG, "Q&A mode: No response received");
        } else {
            ESP_LOGW(TAG, "AUTO mode: Buffer too short (%d bytes)", total);
        }
        return false;
    }
    
    // 查找并处理所有有效数据帧
    uint8_t frame[9];
    bool found_frame = false;
    int last_valid_frame_end = -1;  // 最后一个有效帧的结束位置
    int frames_processed = 0;       // 处理的帧数量
    dart_sensor_data_t temp_data;   // 临时数据，用于存储每一帧的数据
    
    // 循环查找所有帧头0xFF并处理所有有效帧
    for (int i = 0; i <= total - 9; ++i) {
        if (g_rx_buf[i] == 0xFF) {
            memcpy(frame, g_rx_buf + i, 9);
            if (dart_sensor_process_frame(frame, &temp_data)) {
                // 记录有效帧的结束位置
                last_valid_frame_end = i + 9;
                frames_processed++;
                
                // 将最新的有效帧数据复制到输出数据中
                memcpy(data, &temp_data, sizeof(dart_sensor_data_t));
                found_frame = true;
                
                // 在问答模式下，只需要处理第一个有效帧
                if (g_dart_sensor_mode == DART_SENSOR_MODE_QNA) {
                    break;
                }
                // 注意：在AUTO模式下，会继续搜索，以处理所有可能的帧
            }
        }
    }
    
    // 处理缓冲区：如果找到了有效帧，移除所有已处理的数据
    if (found_frame && last_valid_frame_end > 0) {
        // 计算最后一个有效帧后面还剩余的数据量
        int remaining_data = total - last_valid_frame_end;
        if (remaining_data > 0) {
            // 将最后一个有效帧后面的数据移到缓冲区前面
            memmove(g_rx_buf, g_rx_buf + last_valid_frame_end, remaining_data);
            // 更新缓冲区位置
            g_rx_buf_pos = remaining_data;
            // 清空移动后的未使用部分
            memset(g_rx_buf + g_rx_buf_pos, 0, sizeof(g_rx_buf) - g_rx_buf_pos);
            ESP_LOGI(TAG, "Processed %d frames, %d bytes remain in buffer", frames_processed, g_rx_buf_pos);
        } else {
            // 没有剩余数据，清空整个缓冲区
            g_rx_buf_pos = 0;
            memset(g_rx_buf, 0, sizeof(g_rx_buf));
            ESP_LOGI(TAG, "Processed %d frames, buffer cleared", frames_processed);
        }
    } else if (g_dart_sensor_mode == DART_SENSOR_MODE_QNA) {
        // 在问答模式下，如果没有找到有效帧，清空缓冲区准备下一次查询
        g_rx_buf_pos = 0;
        memset(g_rx_buf, 0, sizeof(g_rx_buf));
        ESP_LOGW(TAG, "Q&A mode: No valid frame found");
    } else {
        // 在AUTO模式下，如果没有找到有效帧，保留数据以便下次继续查找
        ESP_LOGW(TAG, "AUTO mode: No valid frame in %d bytes", g_rx_buf_pos);
    }
    
    // 没有有效帧则返回false
    return found_frame;
}

static void dart_sensor_produce_task(void *pvParameters) {
    ESP_LOGI(TAG, "Dart sensor produce task started");
    
    // 初始化传感器模式 - 发送模式切换命令
    dart_sensor_init_mode();
    
    // 在主动上传模式下，等待传感器启动并开始发送数据
    // 在问答模式下，准备开始发送查询
    if (g_dart_sensor_mode == DART_SENSOR_MODE_AUTO) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // 等待传感器开始自动上传
        ESP_LOGI(TAG, "Waiting for sensor to start auto uploading");
    }
    
    dart_sensor_data_t data;
    TickType_t last_read_time = xTaskGetTickCount();
    
    while (1) {
        // 读取传感器数据
        bool data_valid = dart_sensor_read(&data);
        
        // 如果数据有效，则发送到队列
        if (data_valid) {
            xQueueSend(dart_sensor_queue, &data, portMAX_DELAY);
            
            // 更新最后成功读取时间
            last_read_time = xTaskGetTickCount();
        } else {           
            // 如果长时间没有有效数据，可能需要重新初始化模式
            if ((xTaskGetTickCount() - last_read_time) > pdMS_TO_TICKS(10000)) {  // 10秒无数据
                ESP_LOGW(TAG, "No valid data for 10 seconds, re-initializing sensor mode");
                dart_sensor_init_mode();
                last_read_time = xTaskGetTickCount();
            }
        }
        
        // 等待时间根据模式调整
        TickType_t delay_time = (g_dart_sensor_mode == DART_SENSOR_MODE_QNA) ? 
                                pdMS_TO_TICKS(1000) : pdMS_TO_TICKS(1000);
        vTaskDelay(delay_time);
    }
}

static void dart_sensor_consumer_task(void *pvParameters) {
    dart_sensor_data_t data;
    while (1) {
        if (xQueueReceive(dart_sensor_queue, &data, portMAX_DELAY) == pdTRUE) {
            g_ch2o_mg = data.ch2o_ugm3 * 0.001f;
            g_ch2o_timestamp = data.timestamp;
            ESP_LOGI(TAG, "Queue received: %.3f mg/m3, timestamp: %lu s", g_ch2o_mg, (unsigned long)g_ch2o_timestamp);
        }   
        vTaskDelay(pdMS_TO_TICKS(10)); // 避免任务饥饿
    }
}


void dart_sensor_start(void)
{
    dart_sensor_init();
    if (!dart_sensor_queue) {
        dart_sensor_queue = xQueueCreate(8, sizeof(dart_sensor_data_t));
    }
    // 增加任务栈大小，避免栈溢出
    xTaskCreate(dart_sensor_produce_task, "dart_sensor_produce_task", 3072, NULL, 5, NULL);
    xTaskCreate(dart_sensor_consumer_task, "dart_sensor_consumer_task", 2048, NULL, 4, NULL);
}