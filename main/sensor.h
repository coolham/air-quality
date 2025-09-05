#ifndef __SENSOR_H__
#define __SENSOR_H__

#include <stdint.h>

typedef struct {
    float ch2o_ugm3;   // 甲醛浓度，单位：ug/m3
    float ch2o_ppb;    // 甲醛浓度，单位：ppb
    uint32_t timestamp; // 时间戳，单位：秒（可用esp_timer_get_time()/1000000）
    uint32_t count;     // 计数
} hcho_sensor_data_t;

#endif // __SENSOR_H__
