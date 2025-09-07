#ifndef MQTT_DEVICE_H
#define MQTT_DEVICE_H

#include "esp_err.h"



// （可选）如果你还需要整体结构体上传
typedef struct {
    float dart_hcho_mg;
    float dart_hcho_ppb;
    float winsen_hcho_mg;
    float winsen_hcho_ppb;
} air_quality_data_t;


// 初始化MQTT客户端，连接服务器
esp_err_t mqtt_device_start(void);

// 发布 传感器数据
esp_err_t mqtt_device_publish_sensor(const char* sensor_id, const char* sensor_type, float mg, float ppb);


#endif // MQTT_DEVICE_H
