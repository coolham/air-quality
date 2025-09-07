#include "freertos/event_groups.h"
EventBits_t wifi_get_event_bits(void);
#ifndef __WIFI_STATION_H__
#define __WIFI_STATION_H__

void wifi_init_sta(void);

#endif // __WIFI_STATION_H__
