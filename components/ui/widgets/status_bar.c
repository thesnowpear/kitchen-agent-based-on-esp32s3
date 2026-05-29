// 冰箱小精灵 UI 状态栏。
// 显示本地时间、Wi-Fi 状态和电池占位；当前硬件无电池采样，电量仅为 UI 预留。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <time.h>

static lv_obj_t *s_time;
static lv_obj_t *s_wifi;
static lv_obj_t *s_wifi_bars[3];
static lv_obj_t *s_wifi_cross;
static lv_obj_t *s_back;
static lv_obj_t *s_back_label;
static lv_obj_t *s_panel;
static lv_obj_t *s_panel_labels[5];

static void panel_set_visible(bool visible)
{
    if (!s_panel) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_panel);
    } else {
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void panel_cb(lv_event_t *event)
{
    (void)event;
    if (s_panel) {
        panel_set_visible(lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN));
    }
}

static void back_cb(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    fridge_ui_go_back();
}

lv_obj_t *fridge_ui_status_bar_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 28, 18);
    lv_obj_set_size(bar, FRIDGE_DISPLAY_WIDTH - 56, 54);
    lv_obj_set_style_bg_color(bar, theme->surface, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 24, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, theme->line, 0);
    lv_obj_set_style_pad_left(bar, 18, 0);
    lv_obj_set_style_pad_right(bar, 14, 0);
    lv_obj_add_event_cb(bar, panel_cb, LV_EVENT_CLICKED, NULL);

    // 顶栏左侧兼做参考原型的“小精灵 / 返回”按钮，便于位置编辑等流程快速回退。
    s_back = lv_button_create(bar);
    lv_obj_set_size(s_back, 188, 54);
    lv_obj_align(s_back, LV_ALIGN_LEFT_MID, -18, 0);
    lv_obj_set_style_bg_opa(s_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(s_back, 0, 0);
    lv_obj_add_event_cb(s_back, back_cb, LV_EVENT_CLICKED, NULL);
    s_back_label = lv_label_create(s_back);
    lv_obj_set_style_text_color(s_back_label, theme->accent, 0);
    lv_obj_set_style_text_font(s_back_label, fridge_ui_font_body(), 0);
    lv_obj_align(s_back_label, LV_ALIGN_LEFT_MID, 18, 0);

    s_time = lv_label_create(bar);
    lv_obj_set_style_text_color(s_time, theme->text, 0);
    lv_obj_set_style_text_font(s_time, fridge_ui_font_body(), 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_time, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_time, panel_cb, LV_EVENT_CLICKED, NULL);

    // 右侧仅保留 Wi-Fi 状态和信号图标，不再显示电量占位，避免顶栏数字挤压。
    s_wifi = lv_label_create(bar);
    lv_label_set_text(s_wifi, "Wi-Fi");
    lv_obj_set_width(s_wifi, 72);
    lv_label_set_long_mode(s_wifi, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_wifi, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -68, 1);
    lv_obj_set_style_text_color(s_wifi, theme->accent, 0);
    lv_obj_set_style_text_font(s_wifi, fridge_ui_font_body(), 0);
    lv_obj_add_flag(s_wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wifi, panel_cb, LV_EVENT_CLICKED, NULL);

    for (uint8_t i = 0; i < 3; i++) {
        s_wifi_bars[i] = lv_obj_create(bar);
        lv_obj_remove_style_all(s_wifi_bars[i]);
        lv_obj_set_size(s_wifi_bars[i], 8, 8 + i * 7);
        lv_obj_set_style_bg_color(s_wifi_bars[i], theme->line, 0);
        lv_obj_set_style_bg_opa(s_wifi_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_wifi_bars[i], 4, 0);
        lv_obj_align(s_wifi_bars[i], LV_ALIGN_RIGHT_MID, -42 + i * 13, 7 - i * 3);
        lv_obj_add_flag(s_wifi_bars[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_wifi_bars[i], panel_cb, LV_EVENT_CLICKED, NULL);
    }

    // 未连接时显示一个明确的叉号；连接成功后再切回三格信号条。
    s_wifi_cross = lv_label_create(bar);
    lv_label_set_text(s_wifi_cross, "x");
    lv_obj_set_style_text_color(s_wifi_cross, theme->danger, 0);
    lv_obj_set_style_text_font(s_wifi_cross, fridge_ui_font_body(), 0);
    lv_obj_align(s_wifi_cross, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_add_flag(s_wifi_cross, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wifi_cross, panel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_wifi_cross, LV_OBJ_FLAG_HIDDEN);

    s_panel = lv_obj_create(parent);
    lv_obj_set_pos(s_panel, 48, 78);
    lv_obj_set_size(s_panel, 624, 182);
    lv_obj_set_style_bg_color(s_panel, theme->surface, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, theme->line, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_radius(s_panel, 18, 0);
    lv_obj_set_style_shadow_width(s_panel, 16, 0);
    lv_obj_set_style_shadow_opa(s_panel, LV_OPA_20, 0);
    // 展开面板后它会盖在状态栏之上，因此面板本身也响应点击收起。
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_panel, panel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *panel_title = lv_label_create(s_panel);
    lv_label_set_text(panel_title, "设备状态");
    lv_obj_set_style_text_color(panel_title, theme->accent, 0);
    lv_obj_set_style_text_font(panel_title, fridge_ui_font_body(), 0);
    lv_obj_set_pos(panel_title, 18, 8);

    for (uint8_t i = 0; i < 5; i++) {
        s_panel_labels[i] = lv_label_create(s_panel);
        lv_obj_set_style_text_color(s_panel_labels[i], i == 0 ? theme->text : theme->muted, 0);
        lv_obj_set_style_text_font(s_panel_labels[i], fridge_ui_font_small(), 0);
        lv_obj_set_pos(s_panel_labels[i], 18 + (i % 2) * 300, 48 + (i / 2) * 38);
    }
    return bar;
}

void fridge_ui_status_bar_update(void)
{
    if (!s_time) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    char time_text[16] = {0};
    if (tm_now.tm_year >= 120) {
        strftime(time_text, sizeof(time_text), "%H:%M", &tm_now);
    } else {
        snprintf(time_text, sizeof(time_text), "--:--");
    }
    fridge_ui_label_set_text_if_changed(s_time, time_text);

    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    bool wifi_connected = model.network.connected;
    uint8_t wifi_level = 0;
    if (model.network.connected) {
        wifi_level = model.network.rssi >= -60 ? 3 : (model.network.rssi >= -75 ? 2 : 1);
    }
    fridge_ui_label_set_text_if_changed(s_wifi, "Wi-Fi");
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    for (uint8_t i = 0; i < 3; i++) {
        if (!s_wifi_bars[i]) {
            continue;
        }
        if (wifi_connected) {
            lv_obj_clear_flag(s_wifi_bars[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_wifi_bars[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(s_wifi_bars[i], i < wifi_level ? theme->accent : theme->line, 0);
    }
    if (s_wifi_cross) {
        if (wifi_connected) {
            lv_obj_add_flag(s_wifi_cross, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_wifi_cross, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bool can_back = fridge_ui_model_is_place_picking() ||
                    (g_ui_page != FRIDGE_UI_PAGE_HOME && g_ui_page != FRIDGE_UI_PAGE_STANDBY);
    fridge_ui_label_set_text_if_changed(s_back_label, can_back ? "< 返回" : "小精灵");

    if (g_ui_page == FRIDGE_UI_PAGE_STANDBY) {
        panel_set_visible(false);
    }
    if (s_panel_labels[0]) {
        fridge_ui_label_set_text_fmt_if_changed(s_panel_labels[0],
                                                "Wi-Fi  %s",
                                                model.network.connected ? model.network.ssid : (model.network.connecting ? "连接中" : "未连接"));
        fridge_ui_label_set_text_fmt_if_changed(s_panel_labels[1], "信号  %d dBm", model.network.rssi);
        fridge_ui_label_set_text_if_changed(s_panel_labels[2], model.sensors.ready ? "传感器  正常" : "传感器  初始化中");
        fridge_ui_label_set_text_fmt_if_changed(s_panel_labels[3],
                                                "雷达  %s",
                                                model.sensors.radar_stable_presence ? "有人" : "无人");
        fridge_ui_label_set_text_fmt_if_changed(s_panel_labels[4],
                                                "库存  %u 件",
                                                (unsigned)model.food_count);
    }
}
