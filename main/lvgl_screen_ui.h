#ifndef LVGL_SCREEN_UI_H
#define LVGL_SCREEN_UI_H

#include "lvgl.h"

void lvgl_main_ui(lv_display_t *disp);
// void lvgl_port_task(void *arg);

void lvgl_update_dart_ch2o(lv_disp_t *disp, float mg, float ppb);
void lvgl_update_winsen_ch2o(lv_disp_t *disp, float mg, float ppb);

#endif // LVGL_SCREEN_UI_H
