#ifndef __DART_SENSOR_H__
#define __DART_SENSOR_H__

#include <stdint.h>



extern float g_dart_hcho_mg;
extern uint32_t g_dart_hcho_timestamp;

void dart_sensor_init(void);
void dart_sensor_start(void); // 启动传感器任务和打印任务

#endif // __DART_SENSOR_H__