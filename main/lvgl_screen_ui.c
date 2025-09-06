#include <stdio.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "lvgl.h"
#include "esp_log.h"
#include "lvgl_screen_ui.h"

static const char *TAG = "screen";


#define AIR_LVGL_TICK_PERIOD_MS    5
#define AIR_LVGL_TASK_STACK_SIZE   (4 * 1024)
#define AIR_LVGL_TASK_PRIORITY     2
#define AIR_LVGL_PALETTE_SIZE      8
#define AIR_LVGL_TASK_MAX_DELAY_MS 500
#define AIR_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ



static lv_obj_t *dart_hcho_label = NULL;
static lv_obj_t *winsen_hcho_label = NULL;

// To use LV_COLOR_FORMAT_I1, we need an extra buffer to hold the converted data
static uint8_t oled_buffer[AIR_LCD_H_RES * AIR_LCD_V_RES / 8];


extern esp_lcd_panel_handle_t panel_handle;
extern esp_lcd_panel_io_handle_t io_handle;

extern float g_dart_hcho_mg, g_dart_hcho_ppb;
extern float g_winsen_hcho_mg, g_winsen_hcho_ppb;

// LVGL library is not thread-safe, this example will call LVGL APIs from different tasks, so use a mutex to protect it
static _lock_t lvgl_api_lock;


static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t io_panel, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void display_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);

    // This is necessary because LVGL reserves 2 x 4 bytes in the buffer, as these are assumed to be used as a palette. Skip the palette here
    // More information about the monochrome, please refer to https://docs.lvgl.io/9.2/porting/display.html#monochrome-displays
    px_map += AIR_LVGL_PALETTE_SIZE;

    uint16_t hor_res = lv_display_get_physical_horizontal_resolution(disp);
    int x1 = area->x1;
    int x2 = area->x2;
    int y1 = area->y1;
    int y2 = area->y2;

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            /* The order of bits is MSB first
                        MSB           LSB
               bits      7 6 5 4 3 2 1 0
               pixels    0 1 2 3 4 5 6 7
                        Left         Right
            */
            bool chroma_color = (px_map[(hor_res >> 3) * y  + (x >> 3)] & 1 << (7 - x % 8));

            /* Write to the buffer as required for the display.
            * It writes only 1-bit for monochrome displays mapped vertically.*/
            uint8_t *buf = oled_buffer + hor_res * (y >> 3) + (x);
            if (chroma_color) {
                (*buf) &= ~(1 << (y % 8));
            } else {
                (*buf) |= (1 << (y % 8));
            }
        }
    }
    // pass the draw buffer to the driver
    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, oled_buffer);
}

static void display_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(AIR_LVGL_TICK_PERIOD_MS);
}

static void lvgl_port_task(void *arg)
{
    lv_disp_t *display = (lv_disp_t *)arg;
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        // 在主循环中刷新甲醛浓度显示
        lvgl_update_dart_ch2o(display, g_dart_hcho_mg, g_dart_hcho_ppb);
        lvgl_update_winsen_ch2o(display, g_winsen_hcho_mg, g_winsen_hcho_ppb);
        _lock_release(&lvgl_api_lock);
        // in case of triggering a task watch dog time out
        time_till_next_ms = MAX(time_till_next_ms, AIR_LVGL_TASK_MIN_DELAY_MS);
        // in case of lvgl display not ready yet
        time_till_next_ms = MIN(time_till_next_ms, AIR_LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}


esp_err_t init_lvgl_display(void)
{
    ESP_LOGI(TAG, "Initialize LVGL");
    
    lv_init();
    
    // create a lvgl display
    lv_disp_t *display = lv_display_create(AIR_LCD_H_RES, AIR_LCD_V_RES);
    // associate the i2c panel handle to the display
    lv_display_set_user_data(display, panel_handle);
    
    // create draw buffer
    void *buf = NULL;
    ESP_LOGI(TAG, "Allocate separate LVGL draw buffers");
    // LVGL reserves 2 x 4 bytes in the buffer, as these are assumed to be used as a palette.
    size_t draw_buffer_sz = AIR_LCD_H_RES * AIR_LCD_V_RES / 8 + AIR_LVGL_PALETTE_SIZE;
    buf = heap_caps_calloc(1, draw_buffer_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(buf);

    // LVGL9 suooprt new monochromatic format.
    lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
    // initialize LVGL draw buffers
    lv_display_set_buffers(display, buf, NULL, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_FULL);
    // set the callback which can copy the rendered image to an area of the display
    lv_display_set_flush_cb(display, display_lvgl_flush_cb);

    ESP_LOGI(TAG, "Register io panel event callback for LVGL flush ready notification");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    /* Register done callback */
    esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display);

    ESP_LOGI(TAG, "Use esp_timer as LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &display_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, AIR_LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreate(lvgl_port_task, "LVGL", AIR_LVGL_TASK_STACK_SIZE, display, AIR_LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Display LVGL Scroll Text");
    // Lock the mutex due to the LVGL APIs are not thread-safe
    _lock_acquire(&lvgl_api_lock);
    lvgl_main_ui(display);
    _lock_release(&lvgl_api_lock);

    return ESP_OK;
}

void lvgl_update_dart_ch2o(lv_display_t *disp, float mg, float ppb)
{
    static float last_mg = -1.0f;
    if (dart_hcho_label && mg != last_mg) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Dart HCHO: %.3f mg/m3, %d ppb", mg, (int)ppb);
        lv_label_set_text(dart_hcho_label, buf);
        last_mg = mg;
    }
}

void lvgl_update_winsen_ch2o(lv_display_t *disp, float mg, float ppb)
{
    static float last_mg = -1.0f;
    if (winsen_hcho_label && mg != last_mg) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Winsen HCHO: %.3f mg/m3, %d ppb", mg, (int)ppb);
        lv_label_set_text(winsen_hcho_label, buf);
        last_mg = mg;
    }
}


void lvgl_main_ui(lv_display_t *disp)
{
    ESP_LOGI(TAG, "lvgl_main_ui");
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    
    lv_obj_t *label = lv_label_create(scr);

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

