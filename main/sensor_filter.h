#ifndef SENSOR_FILTER_H
#define SENSOR_FILTER_H



#define SENSOR_FILTER_WINDOW_SIZE   6

typedef struct {
    float buf[SENSOR_FILTER_WINDOW_SIZE];
    int idx;
    int count;
} sensor_filter_t;

void sensor_filter_init(sensor_filter_t *filter);
void sensor_filter_update(sensor_filter_t *filter, float value);
float sensor_filter_get(const sensor_filter_t *filter);

#endif // SENSOR_FILTER_H
