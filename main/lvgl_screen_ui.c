/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "lvgl.h"
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "screen";



static lv_obj_t *dart_hcho_label = NULL;
static lv_obj_t *winsen_hcho_label = NULL;


extern float g_dart_hcho_mg;
extern float g_winsen_hcho_mg;

void lvgl_update_dart_ch2o(lv_display_t *disp, float mg, float ppb)
{
    static float last_mg = -1.0f;
    if (dart_hcho_label && mg != last_mg) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Dart HCHO: %.3f mg/m3, %.1f ppb", mg, ppb);
        lv_label_set_text(dart_hcho_label, buf);
        last_mg = mg;
    }
}

void lvgl_update_winsen_ch2o(lv_display_t *disp, float mg, float ppb)
{
    static float last_mg = -1.0f;
    if (winsen_hcho_label && mg != last_mg) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Winsen HCHO: %.3f mg/m3, %.1f ppb", mg, ppb);
        lv_label_set_text(winsen_hcho_label, buf);
        last_mg = mg;
    }
}


void lvgl_main_ui(lv_display_t *disp)
{
    ESP_LOGI(TAG, "lvgl_main_ui");
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    
    lv_obj_t *label = lv_label_create(scr);
    // lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR); /* Circular scroll */
    lv_label_set_text(label, " Air Quality:  ");
    /* Size of the screen (if you use rotation 90 or 270, please use lv_display_get_vertical_resolution) */
    lv_obj_set_width(label, lv_display_get_horizontal_resolution(disp));
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);

    /* 创建Dart甲醛数据label */
    dart_hcho_label = lv_label_create(scr);
    lv_label_set_long_mode(dart_hcho_label, LV_LABEL_LONG_SCROLL_CIRCULAR); /* Circular scroll */
    // 内容必须足够长才能触发滚动，建议初始内容加长
    lv_label_set_text(dart_hcho_label, " HCHO: -- mg/m3 - Real-time Formaldehyde, this is a long test string for scrolling!");
    // 先设置内容，再设置宽度，保证滚动逻辑
    lv_obj_set_width(dart_hcho_label, lv_display_get_horizontal_resolution(disp));
    lv_obj_align(dart_hcho_label, LV_ALIGN_TOP_MID, 0, 20);
    // 设置滚动速度（单位：ms，完整滚动一轮的时间，数值越大越慢）
    lv_obj_set_style_anim_time(dart_hcho_label, 8000, 0);


    // 创建Winsen甲醛数据label
    winsen_hcho_label = lv_label_create(scr);
    lv_label_set_long_mode(winsen_hcho_label, LV_LABEL_LONG_SCROLL_CIRCULAR); /* Circular scroll */
    lv_label_set_text(winsen_hcho_label, " HCHO: -- mg/m3 - Winsen Sensor");

    lv_obj_set_width(winsen_hcho_label, lv_display_get_horizontal_resolution(disp));
    lv_obj_align(winsen_hcho_label, LV_ALIGN_TOP_MID, 0, 40);
    // 设置滚动速度
    lv_obj_set_style_anim_time(winsen_hcho_label, 8000, 0);
}

