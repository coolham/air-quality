#ifndef __DART_SENSOR_H__
#define __DART_SENSOR_H__

#include <stdint.h>

typedef struct {
    float ch2o_ugm3;   // 甲醛浓度，单位：ug/m3
    float ch2o_ppb;    // 甲醛浓度，单位：ppb
    uint32_t timestamp; // 时间戳，单位：秒（可用esp_timer_get_time()/1000000）
} dart_sensor_data_t;

void dart_sensor_init(void);
void dart_sensor_read(dart_sensor_data_t *data);
void dart_sensor_start(void); // 启动传感器任务和打印任务

#endif // __DART_SENSOR_H__