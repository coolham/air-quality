#ifndef __WINSEN_SENSOR_H__
#define __WINSEN_SENSOR_H__

#include <stdint.h>

extern float g_dart_hcho_mg;
extern uint32_t g_dart_hcho_timestamp;

void winsen_sensor_init(void);
void winsen_sensor_start(void); 

#endif // __WINSEN_SENSOR_H__