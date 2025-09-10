
// 通用滑动平均滤波器实现
#include <string.h>
#include "sensor_filter.h"



/**
 * @brief 初始化滑动平均滤波器
 *
 * @param filter 指向滤波器结构体的指针
 *
 * 将滤波器的缓冲区、索引和计数全部清零。
 */
void sensor_filter_init(sensor_filter_t *filter) {
    memset(filter, 0, sizeof(sensor_filter_t));
}


/**
 * @brief 向滤波器输入新数据，更新滑动窗口
 *
 * @param filter 指向滤波器结构体的指针
 * @param value  新采集到的原始数据
 *
 * 将新值写入当前索引位置，索引递增并循环，计数不超过窗口大小。
 */
void sensor_filter_update(sensor_filter_t *filter, float value) {
    filter->buf[filter->idx] = value;
    filter->idx = (filter->idx + 1) % SENSOR_FILTER_WINDOW_SIZE;
    if (filter->count < SENSOR_FILTER_WINDOW_SIZE) filter->count++;
}


/**
 * @brief 获取当前滑动窗口内的平均值（滤波结果）
 *
 * @param filter 指向滤波器结构体的指针
 * @return float 当前窗口内所有数据的平均值，若无数据则返回0
 */
float sensor_filter_get(const sensor_filter_t *filter) {
    float sum = 0.0f;
    for (int i = 0; i < filter->count; ++i) {
        sum += filter->buf[i];
    }
    return (filter->count > 0) ? (sum / filter->count) : 0.0f;
}
