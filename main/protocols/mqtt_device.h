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

// 发布 Dart 传感器数据
esp_err_t mqtt_device_publish_dart(float mg, float ppb);
// 发布 Winsen 传感器数据
esp_err_t mqtt_device_publish_winsen(float mg, float ppb);


esp_err_t mqtt_device_publish_air_quality(const air_quality_data_t *data);



#endif // MQTT_DEVICE_H
