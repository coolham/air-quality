#ifndef LVGL_SCREEN_UI_H
#define LVGL_SCREEN_UI_H

#include "lvgl.h"

// The pixel number in horizontal and vertical
#if CONFIG_LCD_CONTROLLER_SSD1306
#define AIR_LCD_H_RES              128
#define AIR_LCD_V_RES              CONFIG_SSD1306_HEIGHT
#elif CONFIG_LCD_CONTROLLER_SH1107
#define AIR_LCD_H_RES              64
#define AIR_LCD_V_RES              128
#endif


esp_err_t init_lvgl_display(void);
void lvgl_main_ui(lv_display_t *disp);


void lvgl_update_dart_ch2o(lv_disp_t *disp, float mg, float ppb);
void lvgl_update_winsen_ch2o(lv_disp_t *disp, float mg, float ppb);

#endif // LVGL_SCREEN_UI_H
