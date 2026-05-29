// 冰箱小精灵 LVGL UI 内部接口。
// 页面、状态栏和 Dock 通过这些轻量接口协作；跨任务更新必须走 LVGL timer/async，不直接操作对象。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fridge_ui.h"
#include "fridge_display.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_UI_ZONE_COUNT 6
#define FRIDGE_UI_ZONE_CELL_COUNT 9
#define FRIDGE_UI_MAX_WIFI_APS 8
#define FRIDGE_UI_MAX_FOODS (FRIDGE_UI_ZONE_COUNT * FRIDGE_UI_ZONE_CELL_COUNT)

typedef struct {
    char name[24];
    char quantity[16];
    char expire[16];
    char location[32];
    int days_left;
    uint8_t zone;
    uint8_t cell;
} fridge_ui_food_t;

typedef struct {
    char name[16];
    char note[16];
    uint8_t count;
    uint8_t width;
    uint8_t height;
    bool custom;
} fridge_ui_zone_summary_t;

typedef struct {
    bool connected;
    bool connecting;
    bool internet_ready;
    int8_t rssi;
    char ssid[33];
    char ip[16];
    char last_error[96];
} fridge_ui_network_model_t;

typedef struct {
    bool ready;
    uint8_t light_percent;
    bool radar_presence;
    bool radar_stable_presence;
    char state_machine_state[20];
    char door_state[20];
    bool state_offline;
    bool state_is_night;
    bool radar_within_2m;
    bool auto_voice_after_close;
    int64_t updated_at_ms;
} fridge_ui_sensor_model_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secured;
    char authmode[24];
} fridge_ui_wifi_ap_model_t;

typedef struct {
    bool valid;
    bool analyzing;
    bool ok;
    char name[24];
    char quantity[16];
    char meta[256];
    char raw_reply[512];
    char error[128];
    int width;
    int height;
    size_t jpeg_bytes;
    uint32_t latency_ms;
    uint8_t zone;
    uint8_t cell;
} fridge_ui_camera_result_t;

typedef struct {
    fridge_ui_zone_summary_t zones[FRIDGE_UI_ZONE_COUNT];
    fridge_ui_food_t foods[FRIDGE_UI_MAX_FOODS];
    size_t food_count;
    fridge_ui_food_t editing_food;
    bool editing_valid;
    uint8_t active_zone;
    uint8_t active_cell;
    fridge_ui_network_model_t network;
    fridge_ui_sensor_model_t sensors;
    fridge_ui_wifi_ap_model_t wifi_aps[FRIDGE_UI_MAX_WIFI_APS];
    size_t wifi_ap_count;
    bool wifi_scanning;
    char wifi_status[96];
    fridge_ui_camera_result_t camera_result;
    uint8_t brightness;
    uint8_t speaker_volume;
} fridge_ui_model_t;

typedef struct {
    lv_color_t bg;
    lv_color_t surface;
    lv_color_t surface_soft;
    lv_color_t surface_panel;
    lv_color_t text;
    lv_color_t muted;
    lv_color_t accent;
    lv_color_t accent_2;
    lv_color_t danger;
    lv_color_t line;
} fridge_ui_theme_t;

extern lv_obj_t *g_ui_root;
extern fridge_ui_page_t g_ui_page;

void fridge_ui_model_init(void);
void fridge_ui_model_poll(void);
void fridge_ui_model_get(fridge_ui_model_t *out);
void fridge_ui_model_set_active_zone(uint8_t zone);
void fridge_ui_model_select_cell(uint8_t zone, uint8_t cell);
void fridge_ui_model_set_editing_draft(const fridge_ui_food_t *food);
void fridge_ui_model_begin_place_pick(void);
void fridge_ui_model_cancel_place_pick(void);
bool fridge_ui_model_is_place_picking(void);
bool fridge_ui_model_apply_place_pick(uint8_t zone, uint8_t cell);
void fridge_ui_model_update_editing_food(const fridge_ui_food_t *food);
void fridge_ui_model_delete_editing_food(void);
void fridge_ui_model_add_camera_food(const char *name, const char *quantity, uint8_t zone, uint8_t cell);
void fridge_ui_model_set_camera_result(const fridge_ui_camera_result_t *result);
void fridge_ui_model_clear_camera_result(void);
bool fridge_ui_model_add_custom_zone(void);
bool fridge_ui_model_rename_active_zone(const char *name);
bool fridge_ui_model_update_zone(uint8_t zone, const char *name, uint8_t width, uint8_t height, const char *note);
void fridge_ui_model_delete_zone(uint8_t zone);
void fridge_ui_model_delete_active_custom_zone(void);
void fridge_ui_model_start_wifi_scan(void);
void fridge_ui_model_connect_wifi_async(const char *ssid, const char *password);

const fridge_ui_theme_t *fridge_ui_theme_get(void);
void fridge_ui_theme_apply(lv_obj_t *root);
const lv_font_t *fridge_ui_font_small(void);
const lv_font_t *fridge_ui_font_body(void);
const lv_font_t *fridge_ui_font_large(void);
const lv_font_t *fridge_ui_font_title(void);
const lv_font_t *fridge_ui_font_number(void);
const lv_font_t *fridge_ui_font_ai_body(void);
bool fridge_ui_font_covers_text(const lv_font_t *font, const char *text);
const lv_font_t *fridge_ui_font_for_text(const lv_font_t *primary, const char *text);

void fridge_ui_show_page(fridge_ui_page_t page);
void fridge_ui_go_back(void);
void fridge_ui_toast(const char *text);
void fridge_ui_note_activity(void);
void fridge_ui_label_set_text_if_changed(lv_obj_t *label, const char *text);
void fridge_ui_label_set_text_fmt_if_changed(lv_obj_t *label, const char *fmt, ...);

lv_obj_t *fridge_ui_status_bar_create(lv_obj_t *parent);
void fridge_ui_status_bar_update(void);
lv_obj_t *fridge_ui_dock_create(lv_obj_t *parent);
void fridge_ui_dock_update(void);
void fridge_ui_keyboard_open(const char *title, const char *ssid);
void fridge_ui_keyboard_open_text(const char *title, const char *initial_text, void (*done_cb)(const char *text));

void fridge_ui_page_standby_create(lv_obj_t *parent);
void fridge_ui_page_standby_update(void);
void fridge_ui_page_home_create(lv_obj_t *parent);
void fridge_ui_page_home_update(void);
void fridge_ui_page_zone_create(lv_obj_t *parent);
void fridge_ui_page_zone_update(void);
void fridge_ui_page_edit_food_create(lv_obj_t *parent);
void fridge_ui_page_edit_food_update(void);
void fridge_ui_page_door_create(lv_obj_t *parent);
void fridge_ui_page_door_update(void);
void fridge_ui_page_camera_create(lv_obj_t *parent);
void fridge_ui_page_camera_update(void);
void fridge_ui_page_camera_stop(void);
void fridge_ui_page_camera_result_create(lv_obj_t *parent);
void fridge_ui_page_camera_result_update(void);
void fridge_ui_page_recipe_create(lv_obj_t *parent);
void fridge_ui_page_recipe_update(void);
void fridge_ui_page_nutrition_create(lv_obj_t *parent);
void fridge_ui_page_nutrition_update(void);
void fridge_ui_page_ai_create(lv_obj_t *parent);
void fridge_ui_page_ai_update(void);
void fridge_ui_page_shopping_create(lv_obj_t *parent);
void fridge_ui_page_shopping_update(void);
void fridge_ui_page_settings_create(lv_obj_t *parent);
void fridge_ui_page_settings_update(void);
void fridge_ui_page_wifi_create(lv_obj_t *parent);
void fridge_ui_page_wifi_update(void);
void fridge_ui_page_more_create(lv_obj_t *parent);
void fridge_ui_page_more_update(void);
void fridge_ui_page_offline_create(lv_obj_t *parent);
void fridge_ui_page_offline_update(void);
void fridge_ui_page_timer_create(lv_obj_t *parent);
void fridge_ui_page_timer_update(void);
void fridge_ui_page_stopwatch_create(lv_obj_t *parent);
void fridge_ui_page_stopwatch_update(void);
void fridge_ui_page_alarm_create(lv_obj_t *parent);
void fridge_ui_page_alarm_update(void);

#ifdef __cplusplus
}
#endif
