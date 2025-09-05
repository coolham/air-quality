#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "sensor.h"
#include "winsen_sensor.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <time.h>
#include "lvgl_screen_ui.h"
#include "sys/lock.h"

#define WINSEN_UART_PORT_NUM      UART_NUM_2
#define WINSEN_UART_BAUD_RATE    9600
#define WINSEN_UART_TX_PIN       22
#define WINSEN_UART_RX_PIN       23
#define WINSEN_UART_BUF_SIZE     128
#define WINSEN_FRAME_SIZE        9       // WINSEN协议帧长度

static const char *TAG = "winsen_sensor";


// 气体浓度修正系数，默认4，可通过 setter 修改
static float winsen_ch2o_correction_factor = 1.76f;


static QueueHandle_t winsen_sensor_queue = NULL;


float g_winsen_hcho_mg = 0.0f;
uint32_t winsen_ch2o_timestamp = 0;
static uint32_t winsen_dart_read_count = 0;

// Dart协议相关命令
static const uint8_t winsen_cmd_switch_to_qna[9] = {0xFF, 0x01, 0x78, 0x41, 0x00, 0x00, 0x00, 0x00, 0x46};
static const uint8_t winsen_cmd_switch_to_auto[9] = {0xFF, 0x01, 0x78, 0x40, 0x00, 0x00, 0x00, 0x00, 0x47};
static const uint8_t winsen_cmd_read_gas[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};

typedef enum {
    WINSEN_SENSOR_MODE_AUTO = 0, // 自动上传模式
    WINSEN_SENSOR_MODE_QNA  = 1  // 问答模式
} winsen_sensor_mode_t;

static winsen_sensor_mode_t g_winsen_sensor_mode = WINSEN_SENSOR_MODE_QNA;

// 用于存储UART接收数据的静态缓冲区
static uint8_t g_rx_buf[64] = {0};  // 增大缓冲区以容纳更多数据
static int g_rx_buf_pos = 0;  // 当前缓冲区位置，用于追加新数据



void  winsen_set_ch2o_correction_factor(float factor) {
    if (factor > 0.1f && factor < 100.0f) {
        winsen_ch2o_correction_factor = factor;
        ESP_LOGI(TAG, "CH2O correction factor set to %.3f", factor);
    } else {
        ESP_LOGW(TAG, "Invalid correction factor: %.3f, ignored", factor);
    }
}

// 初始化UARET
void winsen_sensor_uart_init(void)
{
    ESP_LOGI(TAG, "Initializing UART for Winsen sensor...");
    const uart_config_t uart_config = {
        .baud_rate = WINSEN_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_driver_install(WINSEN_UART_PORT_NUM, WINSEN_UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(WINSEN_UART_PORT_NUM, &uart_config);
    uart_set_pin(WINSEN_UART_PORT_NUM, WINSEN_UART_TX_PIN, WINSEN_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(TAG, "Winsen sensor UART initialized");

}

// 校验和计算
static uint8_t winsen_checksum(const uint8_t *buf, uint8_t len)
{
    uint8_t sum = 0;
    for (int i = 1; i < len - 1; ++i) {
        sum += buf[i];
    }
    return (~sum) + 1;
}

// 将字节数组转换为可打印的十六进制字符串
static void bytes_to_hex_str(const uint8_t *bytes, int len, char *out_str, int out_str_size)
{
    int pos = 0;
    for (int i = 0; i < len && pos < out_str_size - 4; i++) {
        pos += snprintf(out_str + pos, out_str_size - pos, "%02X ", bytes[i]);
    }
    
    // 确保字符串以null结尾
    if (pos > 0 && pos < out_str_size) {
        out_str[pos-1] = '\0'; // 移除最后一个空格
    } else if (out_str_size > 0) {
        out_str[0] = '\0';
    }
}

/**
 * @brief 向传感器发送数据帧
 * 
 * @param data 要发送的数据帧
 * @param len 数据帧长度
 * @param desc 操作描述，用于日志
 * @return int 实际发送的字节数，小于0表示失败
 */
static int winsen_uart_send(const uint8_t *data, int len, const char *desc)
{
    char hex_str[128] = {0};
    
    // 打印发送的数据
    bytes_to_hex_str(data, len, hex_str, sizeof(hex_str));
    ESP_LOGI(TAG, "UART TX [%s]: %s", desc ? desc : "send", hex_str);
    
    // 发送数据
    int send_bytes = uart_write_bytes(WINSEN_UART_PORT_NUM, (const char*)data, len);
    
    // 检查发送结果
    if (send_bytes != len) {
        ESP_LOGE(TAG, "UART TX Error: Expected to send %d bytes, but sent %d bytes", len, send_bytes);
        return -1;
    }
    
    return send_bytes;
}

/**
 * @brief 从传感器接收数据
 * 
 * @param buf 接收缓冲区
 * @param buf_size 缓冲区大小
 * @param timeout_ms 超时时间(毫秒)
 * @param desc 操作描述，用于日志
 * @return int 实际接收的字节数，0表示超时，小于0表示错误
 */
static int winsen_uart_receive(uint8_t *buf, int buf_size, int timeout_ms, const char *desc)
{
    char hex_str[128] = {0};
    int len = uart_read_bytes(WINSEN_UART_PORT_NUM, buf, buf_size, pdMS_TO_TICKS(timeout_ms));
    
    if (len > 0) {
        // 打印接收到的数据
        bytes_to_hex_str(buf, len > 32 ? 32 : len, hex_str, sizeof(hex_str));
        ESP_LOGI(TAG, "UART RX [%s]: %s%s", desc ? desc : "recv", hex_str, 
                 len > 32 ? "..." : "");
    } else if (len == 0) {
        ESP_LOGD(TAG, "UART RX [%s]: Timeout, no data received in %d ms", 
                 desc ? desc : "recv", timeout_ms);
    } else {
        ESP_LOGE(TAG, "UART RX [%s]: Error %d", desc ? desc : "recv", len);
    }
    
    return len;
}

static void check_inplace_command(void){
    // winsen_cmd_switch_to_qna
    uint8_t checksum = winsen_checksum(winsen_cmd_switch_to_qna, WINSEN_FRAME_SIZE);
    if (checksum != winsen_cmd_switch_to_qna[WINSEN_FRAME_SIZE-1]) {
        ESP_LOGE(TAG, "winsen_cmd_switch_to_qna checksum error");
    }
    // winsen_cmd_switch_to_auto
    checksum = winsen_checksum(winsen_cmd_switch_to_auto, WINSEN_FRAME_SIZE);
    if (checksum != winsen_cmd_switch_to_auto[WINSEN_FRAME_SIZE-1]) {
        ESP_LOGE(TAG, "winsen_cmd_switch_to_auto checksum error");
    }
    // winsen_cmd_read_gas
    checksum = winsen_checksum(winsen_cmd_read_gas, WINSEN_FRAME_SIZE);
    if (checksum != winsen_cmd_read_gas[WINSEN_FRAME_SIZE-1]) {
        ESP_LOGE(TAG, "winsen_cmd_read_gas checksum error");
    }
}


// 初始化传感器工作模式
static void winsen_sensor_init_mode(void)
{
    uint8_t resp_buf[32] = {0};
    int resp_len = 0;
    
    check_inplace_command();

    if (g_winsen_sensor_mode == WINSEN_SENSOR_MODE_QNA) {
        // 验证校验和
        uint8_t checksum = winsen_checksum(winsen_cmd_switch_to_qna, WINSEN_FRAME_SIZE);
        if (checksum != winsen_cmd_switch_to_qna[WINSEN_FRAME_SIZE-1]) {
            ESP_LOGE(TAG, "winsen_cmd_switch_to_qna checksum error");
        }
        
        // 清空接收缓冲区
        uart_flush_input(WINSEN_UART_PORT_NUM);
        
        // 发送切换到QNA模式的命令
        if (winsen_uart_send(winsen_cmd_switch_to_qna, WINSEN_FRAME_SIZE, "switch to QNA mode") != WINSEN_FRAME_SIZE) {
            ESP_LOGE(TAG, "Failed to send switch to QNA mode command");
        }
        
        // 等待短暂时间让传感器处理命令
        vTaskDelay(pdMS_TO_TICKS(20));
        
        // 接收响应
        resp_len = winsen_uart_receive(resp_buf, sizeof(resp_buf), 500, "switch to QNA mode response");

        ESP_LOGI(TAG, "Switching Winsen sensor to Q&A mode, response len: %d", resp_len);
        vTaskDelay(pdMS_TO_TICKS(1000)); // 等待传感器切换模式
    } else {
        // 验证校验和
        uint8_t checksum = winsen_checksum(winsen_cmd_switch_to_auto, WINSEN_FRAME_SIZE);
        if (checksum != winsen_cmd_switch_to_auto[WINSEN_FRAME_SIZE-1]) {
            ESP_LOGE(TAG, "winsen_cmd_switch_to_auto checksum error");
        }
        
        // 清空接收缓冲区
        uart_flush_input(WINSEN_UART_PORT_NUM);
        
        // 发送切换到AUTO模式的命令
        if (winsen_uart_send(winsen_cmd_switch_to_auto, WINSEN_FRAME_SIZE, "switch to AUTO mode") != WINSEN_FRAME_SIZE) {
            ESP_LOGE(TAG, "Failed to send switch to AUTO mode command");
        }
        
        // 等待短暂时间让传感器处理命令
        vTaskDelay(pdMS_TO_TICKS(20));
        
        // 接收响应
        resp_len = winsen_uart_receive(resp_buf, sizeof(resp_buf), 500, "switch to AUTO mode response");

        ESP_LOGI(TAG, "Switching Winsen sensor to AUTO mode, response len: %d", resp_len);
        vTaskDelay(pdMS_TO_TICKS(1000)); // 等待传感器切换模式
    }
};



// 设置数据无效
static void set_data_invalid(hcho_sensor_data_t *data)
{
    data->ch2o_ugm3 = 0.0f;
    data->ch2o_ppb = 0.0f;
    data->timestamp = 0;
    data->count = 0;
}


// 从UART读取原始数据
static int winsen_sensor_read_raw(void)
{
    // 在问答模式下，需要先发送读取命令
    if (g_winsen_sensor_mode == WINSEN_SENSOR_MODE_QNA) {
        // 确保缓冲区是空的，避免读取到旧数据
        uart_flush_input(WINSEN_UART_PORT_NUM);
        
        // 清空接收缓冲区，准备接收新数据
        g_rx_buf_pos = 0;
        memset(g_rx_buf, 0, sizeof(g_rx_buf));
        
        // 发送气体浓度读取命令
        if (winsen_uart_send(winsen_cmd_read_gas, WINSEN_FRAME_SIZE, "read gas concentration") != WINSEN_FRAME_SIZE) {
            ESP_LOGE(TAG, "Failed to send read gas concentration command");
            return 0;
        }
        
        // 等待短暂时间让传感器处理命令
        vTaskDelay(pdMS_TO_TICKS(20));
        
        // 接收响应
        int resp_len = winsen_uart_receive(g_rx_buf, sizeof(g_rx_buf), 1000, "read gas concentration response");

        if (resp_len <= 0) {
            ESP_LOGW(TAG, "QNA mode: No response received after sending command");
            return 0;
        }
        
        // 更新缓冲区位置
        g_rx_buf_pos = resp_len;
        return g_rx_buf_pos;
    } else {
        // 主动上传模式下，直接等待传感器发送数据
        ESP_LOGI(TAG, "Waiting for auto data, buffer pos: %d", g_rx_buf_pos);
        
        // 在AUTO模式下，我们保留之前的数据，可能包含部分帧
        // 如果缓冲区已经快满了，则保留末尾至少18字节（可能的完整帧），丢弃更早的数据
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
    int continuous_empty_reads = 0;  // 连续空读取计数
    
    // 动态超时处理
    // 在问答模式下，等待时间可以短一些，因为发送命令后立即会有响应
    // 在自动上传模式下，需要等待更长时间，因为传感器每秒才发送一次数据
    int max_timeout = (g_winsen_sensor_mode == WINSEN_SENSOR_MODE_QNA) ? 150 : 50; // 增加超时时间，问答模式1.5s，自动模式500ms
    int timeout = max_timeout;
    bool have_frame_header = false;  // 标记是否找到了帧头0xFF
    int frame_start_pos = -1;        // 记录帧头位置
    
    // 检查是否已经有帧头在缓冲区中
    for (int i = 0; i < g_rx_buf_pos; i++) {    
        if (g_rx_buf[i] == 0xFF) {
            have_frame_header = true;
            frame_start_pos = i;
            break;
        }
    }
    
    // 持续读取数据，直到满足以下条件之一：
    // 1. 缓冲区已满
    // 2. 超时
    // 3. 在QNA模式下接收到完整帧(帧头+至少9字节)
    // 4. 连续多次读取都没有新数据
    while (g_rx_buf_pos < sizeof(g_rx_buf) && timeout-- > 0) {
        // 使用winsen_uart_receive函数读取数据
        int len = winsen_uart_receive(g_rx_buf + g_rx_buf_pos, 
                                   sizeof(g_rx_buf) - g_rx_buf_pos, 
                                   100, "auto polling");
                                   
        if (len > 0) {
            g_rx_buf_pos += len;
            continuous_empty_reads = 0;  // 重置空读取计数
            
            // 如果之前没有找到帧头，检查新数据中是否有帧头
            if (!have_frame_header) {
                for (int i = start_pos; i < g_rx_buf_pos; i++) {
                    if (g_rx_buf[i] == 0xFF) {
                        have_frame_header = true;
                        frame_start_pos = i;
                        ESP_LOGI(TAG, "Found frame header at position %d", i);
                        break;
                    }
                }
            }
            
            // 如果找到了帧头，检查是否已经接收了完整的帧(至少9字节)
            if (have_frame_header && (g_rx_buf_pos - frame_start_pos) >= WINSEN_FRAME_SIZE) {
                // 在QNA模式下，只需要一个完整帧就可以结束
                if (g_winsen_sensor_mode == WINSEN_SENSOR_MODE_QNA) {
                    ESP_LOGI(TAG, "Q&A mode: Got complete frame, stopping");
                    break;
                }
                // 在AUTO模式下，等待更多数据，以便可能接收多个帧
                // 但如果已经有一个完整帧，可以减少超时时间，以便更快处理
                timeout = timeout < 10 ? timeout : 10;
            }
        } else {
            continuous_empty_reads++;
            
            // 如果连续多次读取都没有数据，可能没有更多数据了
            if (continuous_empty_reads >= 5) {
                ESP_LOGD(TAG, "No more data after %d continuous empty reads", continuous_empty_reads);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    int new_bytes = g_rx_buf_pos - start_pos;
    
    // 如果接收了很多数据但缓冲区接近已满，可能需要立即处理
    if (new_bytes > 0 && g_rx_buf_pos > sizeof(g_rx_buf) - WINSEN_FRAME_SIZE) {
        ESP_LOGW(TAG, "Buffer nearly full after reading (%d bytes), immediate processing needed", g_rx_buf_pos);
    }
    
    // 日志记录超时或完成接收的原因
    if (timeout <= 0) {
        ESP_LOGW(TAG, "Read timeout reached, received %d new bytes, total: %d bytes", new_bytes, g_rx_buf_pos);
    } else {
        ESP_LOGI(TAG, "Received %d new bytes, total: %d bytes, remaining timeout: %d", 
                 new_bytes, g_rx_buf_pos, timeout);
    }
    
    return g_rx_buf_pos;  // 返回缓冲区中的总数据量
}

// 处理接收到的数据帧
static bool winsen_sensor_process_frame(const uint8_t *frame, hcho_sensor_data_t *data)
{
    uint8_t checksum = winsen_checksum(frame, WINSEN_FRAME_SIZE);
    if (checksum != frame[WINSEN_FRAME_SIZE-1]) {
        char hex_str[64];
        bytes_to_hex_str(frame, WINSEN_FRAME_SIZE, hex_str, sizeof(hex_str));
        ESP_LOGW(TAG, "Checksum error: %02X != %02X, frame: %s", frame[WINSEN_FRAME_SIZE-1], checksum, hex_str);
        set_data_invalid(data);
        return false;
    }
    // 简化日志，减少栈使用
    ESP_LOGD(TAG, "Frame: %02X %02X %02X %02X...", frame[0], frame[1], frame[2], frame[3]);
    
    // 处理不同的帧类型
    if (g_winsen_sensor_mode == WINSEN_SENSOR_MODE_QNA) {
        // 问答模式：期望收到的是读取气体浓度的响应帧 (0x86)
        if (frame[1] == 0x86) {
            // 读取气体浓度帧 (response to 读取气体浓度)
            uint16_t ch2o_ugm3 = frame[2] * 256 + frame[3];  // ug/m3
            uint16_t ch2o_ppb  = frame[6] * 256 + frame[7];  // ppb
            // 修正
            data->ch2o_ugm3 = (float)ch2o_ugm3 / winsen_ch2o_correction_factor;
            data->ch2o_ppb  = (float)ch2o_ppb / winsen_ch2o_correction_factor;
            data->timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            winsen_dart_read_count++;
            data->count = winsen_dart_read_count;
            ESP_LOGI(TAG, "CH2O (Q&A): raw=%u ug/m3, %u ppb, corrected=%.2f ug/m3, %.2f ppb, factor=%.2f", 
                ch2o_ugm3, ch2o_ppb, data->ch2o_ugm3, data->ch2o_ppb, winsen_ch2o_correction_factor);
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
            // 计算实际值并修正
            if (is_ppb) {
                data->ch2o_ppb = (float)gas_value / winsen_ch2o_correction_factor;
                data->ch2o_ugm3 = data->ch2o_ppb * 1.23f;
            } else {
                data->ch2o_ugm3 = (float)gas_value / winsen_ch2o_correction_factor;
                data->ch2o_ppb = data->ch2o_ugm3 / 1.23f;
            }
            data->timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            winsen_dart_read_count++;
            data->count = winsen_dart_read_count;
            ESP_LOGI(TAG, "CH2O (AUTO): raw=%u, corrected=%.2f ug/m3, %.2f ppb, factor=%.2f, full_scale=%u", 
                gas_value, data->ch2o_ugm3, data->ch2o_ppb, winsen_ch2o_correction_factor, full_scale);
            return true;
        } else if (frame[1] == 0x86) {
            // 也可能收到0x86帧，尤其是在切换模式时
            uint16_t ch2o_ugm3 = frame[2] * 256 + frame[3];
            uint16_t ch2o_ppb  = frame[6] * 256 + frame[7];
            data->ch2o_ugm3 = (float)ch2o_ugm3 / winsen_ch2o_correction_factor;
            data->ch2o_ppb  = (float)ch2o_ppb / winsen_ch2o_correction_factor;
            data->timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            winsen_dart_read_count++;
            data->count = winsen_dart_read_count;
            ESP_LOGI(TAG, "CH2O (0x86/AUTO): raw=%u ug/m3, %u ppb, corrected=%.2f ug/m3, %.2f ppb, factor=%.2f", 
                ch2o_ugm3, ch2o_ppb, data->ch2o_ugm3, data->ch2o_ppb, winsen_ch2o_correction_factor);
            return true;
        }
    }
    
    ESP_LOGW(TAG, "Unhandled frame type: 0x%02X", frame[1]);
    set_data_invalid(data);
    return false;
}

// 读取并处理传感器数据
static bool winsen_sensor_read(hcho_sensor_data_t *data)
{
    // 首先清空数据
    set_data_invalid(data);
    
    // 从传感器读取原始数据
    // 在Q&A模式下，dart_sensor_read_raw会发送读取命令，然后等待响应
    // 在AUTO模式下，dart_sensor_read_raw只会等待传感器自动发送的数据
    int total = winsen_sensor_read_raw();
    if (total < 3) {  // 至少需要能检测到帧头和一个字节类型
        if (g_winsen_sensor_mode == WINSEN_SENSOR_MODE_QNA) {
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
    hcho_sensor_data_t temp_data;   // 临时数据，用于存储每一帧的数据
    char hex_str[64];               // 用于日志打印
    
    // 循环查找所有帧头0xFF并处理所有有效帧
    for (int i = 0; i <= total - 3; ++i) {  // 至少需要能检测到帧头和一个字节类型
        if (g_rx_buf[i] == 0xFF) {
            // 检查是否有足够的数据构成一个完整帧(9字节)
            if (i + 9 <= total) {
                // 有足够数据构成完整帧
                memcpy(frame, g_rx_buf + i, 9);
                if (winsen_sensor_process_frame(frame, &temp_data)) {
                    // 记录有效帧的结束位置
                    last_valid_frame_end = i + 9;
                    frames_processed++;
                    
                    // 将最新的有效帧数据复制到输出数据中
                    memcpy(data, &temp_data, sizeof(hcho_sensor_data_t));
                    found_frame = true;
                    
                    // 在问答模式下，只需要处理第一个有效帧
                    if (g_winsen_sensor_mode == WINSEN_SENSOR_MODE_QNA) {
                        break;
                    }
                    // 注意：在AUTO模式下，会继续搜索，以处理所有可能的帧
                } else {
                    // 校验和错误或不支持的帧类型，打印调试信息
                    bytes_to_hex_str(g_rx_buf + i, 9, hex_str, sizeof(hex_str));
                    ESP_LOGD(TAG, "Invalid frame at pos %d: %s", i, hex_str);
                }
            } else {
                // 发现帧头但数据不足以构成完整帧，保留这些数据以便下次读取
                ESP_LOGI(TAG, "Incomplete frame at end of buffer, keeping %d bytes for next read", total - i);
                
                // 如果这是buffer中唯一的帧头，那么将它移到buffer开始位置
                if (i > 0) {
                    memmove(g_rx_buf, g_rx_buf + i, total - i);
                    g_rx_buf_pos = total - i;
                    memset(g_rx_buf + g_rx_buf_pos, 0, sizeof(g_rx_buf) - g_rx_buf_pos);
                }
                
                // 不再继续查找，保留不完整的帧
                break;
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
            
            // 打印剩余数据信息
            if (g_rx_buf_pos > 0) {
                bytes_to_hex_str(g_rx_buf, (g_rx_buf_pos > 16) ? 16 : g_rx_buf_pos, hex_str, sizeof(hex_str));
                ESP_LOGI(TAG, "Processed %d frames, %d bytes remain: %s%s", 
                         frames_processed, g_rx_buf_pos, hex_str, (g_rx_buf_pos > 16) ? "..." : "");
            } else {
                ESP_LOGI(TAG, "Processed %d frames, no bytes remain", frames_processed);
            }
        } else {
            // 没有剩余数据，清空整个缓冲区
            g_rx_buf_pos = 0;
            memset(g_rx_buf, 0, sizeof(g_rx_buf));
            ESP_LOGD(TAG, "Processed %d frames, buffer cleared", frames_processed);
        }
    } else if (g_winsen_sensor_mode == WINSEN_SENSOR_MODE_QNA) {
        // 在问答模式下，如果没有找到有效帧，检查是否有潜在的帧头
        bool has_frame_header = false;
        int latest_header_pos = -1;
        
        for (int i = 0; i < total; i++) {
            if (g_rx_buf[i] == 0xFF) {
                has_frame_header = true;
                latest_header_pos = i;
            }
        }
        
        if (has_frame_header && latest_header_pos + 9 > total) {
            // 有帧头但数据不完整，保留这些数据
            if (latest_header_pos > 0) {
                // 移动帧头到缓冲区开始位置
                memmove(g_rx_buf, g_rx_buf + latest_header_pos, total - latest_header_pos);
                g_rx_buf_pos = total - latest_header_pos;
                memset(g_rx_buf + g_rx_buf_pos, 0, sizeof(g_rx_buf) - g_rx_buf_pos);
                ESP_LOGI(TAG, "Q&A mode: Keeping partial frame (%d bytes)", g_rx_buf_pos);
            } else {
                // 帧头已经在缓冲区开始位置，保持现状
                ESP_LOGI(TAG, "Q&A mode: Keeping partial frame at buffer start (%d bytes)", total);
            }
        } else {
            // 没有帧头或帧头数据已完整但处理失败，清空缓冲区
            g_rx_buf_pos = 0;
            memset(g_rx_buf, 0, sizeof(g_rx_buf));
            ESP_LOGW(TAG, "Q&A mode: No valid frame found");
        }
    } else {
        // 在AUTO模式下，如果没有找到有效帧，检查是否有潜在的帧头
        bool has_frame_header = false;
        int latest_header_pos = -1;
        
        for (int i = 0; i < total; i++) {
            if (g_rx_buf[i] == 0xFF) {
                has_frame_header = true;
                latest_header_pos = i;
                // 不break，找最后一个帧头
            }
        }
        
        if (has_frame_header) {
            // 有帧头，可能是不完整的帧，将它移到缓冲区开始位置
            if (latest_header_pos > 0) {
                memmove(g_rx_buf, g_rx_buf + latest_header_pos, total - latest_header_pos);
                g_rx_buf_pos = total - latest_header_pos;
                memset(g_rx_buf + g_rx_buf_pos, 0, sizeof(g_rx_buf) - g_rx_buf_pos);
                ESP_LOGI(TAG, "AUTO mode: Keeping potential frame start (%d bytes)", g_rx_buf_pos);
            } else {
                // 帧头已经在缓冲区开始位置，保持现状
                ESP_LOGI(TAG, "AUTO mode: Keeping potential frame at buffer start (%d bytes)", total);
            }
        } else {
            // 缓冲区中没有帧头，清空一半缓冲区，保留后半部分以防万一
            int keep_size = total / 2;
            if (keep_size > 0) {
                memmove(g_rx_buf, g_rx_buf + total - keep_size, keep_size);
                g_rx_buf_pos = keep_size;
                memset(g_rx_buf + g_rx_buf_pos, 0, sizeof(g_rx_buf) - g_rx_buf_pos);
                ESP_LOGW(TAG, "AUTO mode: No frame header, keeping last %d bytes", keep_size);
            } else {
                // 缓冲区太小，清空
                g_rx_buf_pos = 0;
                memset(g_rx_buf, 0, sizeof(g_rx_buf));
                ESP_LOGW(TAG, "AUTO mode: No valid data, buffer cleared");
            }
        }
    }
    
    // 没有有效帧则返回false
    return found_frame;
}

static void winsen_sensor_producer_task(void *pvParameters) {
    ESP_LOGI(TAG, "Winsen sensor produce task started");

    // 初始化传感器模式 - 发送模式切换命令
    winsen_sensor_init_mode();
    
    // 在主动上传模式下，等待传感器启动并开始发送数据
    // 在问答模式下，准备开始发送查询
    if (g_winsen_sensor_mode == WINSEN_SENSOR_MODE_AUTO) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // 等待传感器开始自动上传
        ESP_LOGI(TAG, "Waiting for sensor to start auto uploading");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    hcho_sensor_data_t data;
    TickType_t last_read_time = xTaskGetTickCount();
    
    while (1) {
        // 读取传感器数据
        bool data_valid = winsen_sensor_read(&data);
        
        // 如果数据有效，则发送到队列
        if (data_valid) {
            xQueueSend(winsen_sensor_queue, &data, portMAX_DELAY);

            // 更新最后成功读取时间
            last_read_time = xTaskGetTickCount();
        } else {           
            // 如果长时间没有有效数据，可能需要重新初始化模式
            if ((xTaskGetTickCount() - last_read_time) > pdMS_TO_TICKS(10000)) {  // 10秒无数据
                ESP_LOGW(TAG, "No valid data for 10 seconds, re-initializing sensor mode");
                winsen_sensor_init_mode();
                last_read_time = xTaskGetTickCount();
            }
        }
        
        // 等待时间根据模式调整
        TickType_t delay_time = (g_winsen_sensor_mode == WINSEN_SENSOR_MODE_QNA) ? 
                                pdMS_TO_TICKS(5000) : pdMS_TO_TICKS(1000);
        vTaskDelay(delay_time);
    }
}

static void winsen_sensor_consumer_task(void *pvParameters) {
    hcho_sensor_data_t data;
    while (1) {
        if (xQueueReceive(winsen_sensor_queue, &data, portMAX_DELAY) == pdTRUE) {
            g_winsen_hcho_mg = data.ch2o_ugm3 * 0.001f;
            winsen_ch2o_timestamp = data.timestamp;
            ESP_LOGD(TAG, "Queue received: %.3f mg/m3, timestamp: %lu s", g_winsen_hcho_mg, (unsigned long)winsen_ch2o_timestamp);
        }   
        vTaskDelay(pdMS_TO_TICKS(10)); // 避免任务饥饿
    }
}


void winsen_sensor_start(void)
{
    winsen_sensor_uart_init();

    if (!winsen_sensor_queue) {
        winsen_sensor_queue = xQueueCreate(10, sizeof(hcho_sensor_data_t));
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 增加任务栈大小，避免栈溢出
    xTaskCreate(winsen_sensor_producer_task, "winsen_sensor_produce_task", 3072, NULL, 5, NULL);
    xTaskCreate(winsen_sensor_consumer_task, "winsen_sensor_consumer_task", 2048, NULL, 4, NULL);
}