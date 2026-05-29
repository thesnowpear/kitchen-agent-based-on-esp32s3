// 冰箱小精灵 LVGL UI 主任务。
// 负责初始化 TR230S 显示、FT6336U 触摸、LVGL tick/flush/read 回调，并创建五个核心页面。

#include "fridge_ui_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_display.h"
#include "fridge_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UI_TASK_STACK 24576
#define UI_TASK_PRIORITY 5
#define UI_BUFFER_ROWS 120
#define UI_MODEL_POLL_MS 1000
#define UI_TICK_MS 5
#define UI_TOAST_MS 1800

static const char *TAG = "fridge_ui";

lv_obj_t *g_ui_root;
fridge_ui_page_t g_ui_page = FRIDGE_UI_PAGE_STANDBY;

static TaskHandle_t s_ui_task;
static lv_display_t *s_display;
static lv_obj_t *s_pages[FRIDGE_UI_PAGE_COUNT];
static lv_obj_t *s_content;
static lv_obj_t *s_status_bar;
static lv_obj_t *s_dock;
static lv_obj_t *s_toast;
static int64_t s_toast_until_ms;
static int64_t s_last_activity_ms;
static int64_t s_last_model_poll_ms;

static void ui_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(UI_TICK_MS);
}

static void ui_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_err_t ret = fridge_display_flush_area((uint16_t)area->x1,
                                              (uint16_t)area->y1,
                                              (uint16_t)area->x2,
                                              (uint16_t)area->y2,
                                              (const uint16_t *)px_map);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "display flush failed: %s", esp_err_to_name(ret));
    }
    lv_display_flush_ready(disp);
}

static void ui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    fridge_touch_point_t point = {0};
    if (fridge_touch_read(&point) == ESP_OK && point.pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = point.x;
        data->point.y = point.y;
        fridge_ui_note_activity();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static lv_obj_t *make_page(void)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *page = lv_obj_create(s_content);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, theme->bg, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

static void update_current_page(void)
{
    bool standby = g_ui_page == FRIDGE_UI_PAGE_STANDBY;
    if (s_status_bar) {
        if (standby) {
            lv_obj_add_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_dock) {
        if (standby) {
            lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_content) {
        lv_obj_set_pos(s_content, 0, standby ? 0 : 82);
        lv_obj_set_size(s_content, FRIDGE_DISPLAY_WIDTH, standby ? FRIDGE_DISPLAY_HEIGHT : 538);
    }
    fridge_ui_status_bar_update();
    fridge_ui_dock_update();
    switch (g_ui_page) {
    case FRIDGE_UI_PAGE_STANDBY:
        fridge_ui_page_standby_update();
        break;
    case FRIDGE_UI_PAGE_HOME:
        fridge_ui_page_home_update();
        break;
    case FRIDGE_UI_PAGE_ZONE:
        fridge_ui_page_zone_update();
        break;
    case FRIDGE_UI_PAGE_EDIT_FOOD:
        fridge_ui_page_edit_food_update();
        break;
    case FRIDGE_UI_PAGE_DOOR:
        fridge_ui_page_door_update();
        break;
    case FRIDGE_UI_PAGE_CAMERA:
        fridge_ui_page_camera_update();
        break;
    case FRIDGE_UI_PAGE_CAMERA_RESULT:
        fridge_ui_page_camera_result_update();
        break;
    case FRIDGE_UI_PAGE_RECIPE:
        fridge_ui_page_recipe_update();
        break;
    case FRIDGE_UI_PAGE_SHOPPING:
        fridge_ui_page_shopping_update();
        break;
    case FRIDGE_UI_PAGE_SETTINGS:
        fridge_ui_page_settings_update();
        break;
    case FRIDGE_UI_PAGE_WIFI:
        fridge_ui_page_wifi_update();
        break;
    case FRIDGE_UI_PAGE_MORE:
        fridge_ui_page_more_update();
        break;
    case FRIDGE_UI_PAGE_OFFLINE:
        fridge_ui_page_offline_update();
        break;
    default:
        break;
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    ESP_RETURN_VOID_ON_ERROR(fridge_display_init(), TAG, "display init failed");
    esp_err_t touch_ret = fridge_touch_init();
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed, UI continues without touch: %s", esp_err_to_name(touch_ret));
    }

    lv_init();
    size_t buf_pixels = FRIDGE_DISPLAY_WIDTH * UI_BUFFER_ROWS;
    void *buf1 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *buf2 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "LVGL draw buffer allocation failed");
        vTaskDelete(NULL);
    }

    s_display = lv_display_create(FRIDGE_DISPLAY_WIDTH, FRIDGE_DISPLAY_HEIGHT);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, buf1, buf2, buf_pixels * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, ui_flush_cb);
    ESP_LOGI(TAG, "LVGL draw buffers ready, rows=%u bytes_each=%u",
             (unsigned)UI_BUFFER_ROWS,
             (unsigned)(buf_pixels * sizeof(lv_color_t)));

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, ui_touch_read_cb);

    const esp_timer_create_args_t tick_args = {
        .callback = ui_tick_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, UI_TICK_MS * 1000));

    fridge_ui_model_init();
    fridge_ui_model_poll();

    g_ui_root = lv_screen_active();
    fridge_ui_theme_apply(g_ui_root);

    s_status_bar = fridge_ui_status_bar_create(g_ui_root);
    s_content = lv_obj_create(g_ui_root);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_pos(s_content, 0, 82);
    lv_obj_set_size(s_content, FRIDGE_DISPLAY_WIDTH, 538);
    lv_obj_set_style_bg_color(s_content, fridge_ui_theme_get()->bg, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_COVER, 0);

    for (uint8_t i = 0; i < FRIDGE_UI_PAGE_COUNT; i++) {
        s_pages[i] = make_page();
    }

    fridge_ui_page_standby_create(s_pages[FRIDGE_UI_PAGE_STANDBY]);
    fridge_ui_page_home_create(s_pages[FRIDGE_UI_PAGE_HOME]);
    fridge_ui_page_zone_create(s_pages[FRIDGE_UI_PAGE_ZONE]);
    fridge_ui_page_edit_food_create(s_pages[FRIDGE_UI_PAGE_EDIT_FOOD]);
    fridge_ui_page_door_create(s_pages[FRIDGE_UI_PAGE_DOOR]);
    fridge_ui_page_camera_create(s_pages[FRIDGE_UI_PAGE_CAMERA]);
    fridge_ui_page_camera_result_create(s_pages[FRIDGE_UI_PAGE_CAMERA_RESULT]);
    fridge_ui_page_recipe_create(s_pages[FRIDGE_UI_PAGE_RECIPE]);
    fridge_ui_page_shopping_create(s_pages[FRIDGE_UI_PAGE_SHOPPING]);
    fridge_ui_page_settings_create(s_pages[FRIDGE_UI_PAGE_SETTINGS]);
    fridge_ui_page_wifi_create(s_pages[FRIDGE_UI_PAGE_WIFI]);
    fridge_ui_page_more_create(s_pages[FRIDGE_UI_PAGE_MORE]);
    fridge_ui_page_offline_create(s_pages[FRIDGE_UI_PAGE_OFFLINE]);
    s_dock = fridge_ui_dock_create(g_ui_root);

    s_toast = lv_label_create(g_ui_root);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x292520), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_white(), 0);
    lv_obj_set_style_pad_all(s_toast, 14, 0);
    lv_obj_set_style_radius(s_toast, 8, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -132);

    fridge_ui_show_page(FRIDGE_UI_PAGE_STANDBY);
    ESP_LOGI(TAG, "LVGL UI started");

    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_last_model_poll_ms >= UI_MODEL_POLL_MS) {
            fridge_ui_model_poll();
            update_current_page();
            s_last_model_poll_ms = now_ms;
        }
        if (s_toast && s_toast_until_ms > 0 && now_ms >= s_toast_until_ms) {
            lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
            s_toast_until_ms = 0;
        }
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t fridge_ui_init(void)
{
    if (s_ui_task) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "fridge_ui", UI_TASK_STACK, NULL, UI_TASK_PRIORITY, &s_ui_task, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t fridge_ui_set_page(fridge_ui_page_t page)
{
    if (page >= FRIDGE_UI_PAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    fridge_ui_show_page(page);
    return ESP_OK;
}

esp_err_t fridge_ui_set_brightness(uint8_t percent)
{
    return fridge_display_set_brightness(percent);
}

void fridge_ui_request_standby(void)
{
    fridge_ui_show_page(FRIDGE_UI_PAGE_STANDBY);
}

void fridge_ui_show_page(fridge_ui_page_t page)
{
    if (!s_pages[page]) {
        return;
    }
    if (g_ui_page < FRIDGE_UI_PAGE_COUNT && s_pages[g_ui_page] && g_ui_page != page) {
        lv_obj_add_flag(s_pages[g_ui_page], LV_OBJ_FLAG_HIDDEN);
    }
    g_ui_page = page;
    lv_obj_remove_flag(s_pages[page], LV_OBJ_FLAG_HIDDEN);
    fridge_ui_note_activity();
    update_current_page();
}

void fridge_ui_go_back(void)
{
    if (fridge_ui_model_is_place_picking()) {
        fridge_ui_model_cancel_place_pick();
        fridge_ui_show_page(FRIDGE_UI_PAGE_EDIT_FOOD);
        return;
    }
    switch (g_ui_page) {
    case FRIDGE_UI_PAGE_ZONE:
    case FRIDGE_UI_PAGE_DOOR:
    case FRIDGE_UI_PAGE_RECIPE:
    case FRIDGE_UI_PAGE_MORE:
    case FRIDGE_UI_PAGE_OFFLINE:
        fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
        break;
    case FRIDGE_UI_PAGE_EDIT_FOOD:
        fridge_ui_show_page(FRIDGE_UI_PAGE_ZONE);
        break;
    case FRIDGE_UI_PAGE_CAMERA:
        fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
        break;
    case FRIDGE_UI_PAGE_CAMERA_RESULT:
        fridge_ui_show_page(FRIDGE_UI_PAGE_CAMERA);
        break;
    case FRIDGE_UI_PAGE_SETTINGS:
        fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
        break;
    case FRIDGE_UI_PAGE_WIFI:
        fridge_ui_show_page(FRIDGE_UI_PAGE_SETTINGS);
        break;
    default:
        fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
        break;
    }
}

void fridge_ui_toast(const char *text)
{
    if (!s_toast || !text) {
        return;
    }
    fridge_ui_label_set_text_if_changed(s_toast, text);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_until_ms = esp_timer_get_time() / 1000 + UI_TOAST_MS;
}

void fridge_ui_note_activity(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000;
}

void fridge_ui_label_set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) {
        return;
    }
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

void fridge_ui_label_set_text_fmt_if_changed(lv_obj_t *label, const char *fmt, ...)
{
    if (!label || !fmt) {
        return;
    }
    char buf[160] = {0};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fridge_ui_label_set_text_if_changed(label, buf);
}
