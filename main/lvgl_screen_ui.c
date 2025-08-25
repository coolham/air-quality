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



static lv_obj_t *hcho_label = NULL;

extern float g_ch2o_mg;

void lvgl_update_ch2o(float mg, float ppb)
{
    if (hcho_label) {
        char buf[128];
        // 创建更长的文本以触发滚动效果，只显示mg/m3值
        snprintf(buf, sizeof(buf), " HCHO: %.3f mg/m3 - Real-time Formaldehyde", mg);
        lv_label_set_text(hcho_label, buf);
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

    /* 创建甲醛数据label */
    hcho_label = lv_label_create(scr);
    lv_label_set_long_mode(hcho_label, LV_LABEL_LONG_SCROLL_CIRCULAR); /* Circular scroll */
    lv_label_set_text(hcho_label, " HCHO: -- mg/m3 - Real-time Formaldehyde");

    lv_obj_set_width(hcho_label, lv_display_get_horizontal_resolution(disp));
    lv_obj_align(hcho_label, LV_ALIGN_TOP_MID, 0, 20);
}

