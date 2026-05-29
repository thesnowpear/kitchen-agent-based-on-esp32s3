// 冰箱小精灵 Wi-Fi 配网页。
// 扫描在后台任务执行，页面只展示结果；选择热点后打开密码键盘，连接结果写回状态文案。

#include "fridge_ui_internal.h"

#include <stdint.h>
#include <string.h>

static lv_obj_t *s_status;
static lv_obj_t *s_list;
static char s_last_status[96];
static size_t s_last_ap_count = SIZE_MAX;
static bool s_last_scanning;

static void refresh_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_model_start_wifi_scan();
}

static void ap_cb(lv_event_t *event)
{
    const char *ssid = lv_event_get_user_data(event);
    if (ssid) {
        fridge_ui_keyboard_open("Wi-Fi", ssid);
    }
}

void fridge_ui_page_wifi_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Wi-Fi");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 22);

    lv_obj_t *refresh = lv_button_create(parent);
    lv_obj_set_size(refresh, 150, 56);
    lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, -36, 18);
    lv_obj_set_style_bg_color(refresh, theme->accent, 0);
    lv_obj_set_style_radius(refresh, 8, 0);
    lv_obj_set_style_shadow_width(refresh, 0, 0);
    lv_obj_add_event_cb(refresh, refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh_label = lv_label_create(refresh);
    lv_label_set_text(refresh_label, "刷新");
    lv_obj_center(refresh_label);

    s_status = lv_label_create(parent);
    lv_obj_set_style_text_color(s_status, theme->muted, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 42, 86);

    s_list = lv_obj_create(parent);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_pos(s_list, 36, 126);
    lv_obj_set_size(s_list, 648, 382);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

void fridge_ui_page_wifi_update(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    fridge_ui_label_set_text_if_changed(s_status, model.wifi_status);

    if (s_last_ap_count == model.wifi_ap_count &&
        s_last_scanning == model.wifi_scanning &&
        strcmp(s_last_status, model.wifi_status) == 0) {
        return;
    }
    s_last_ap_count = model.wifi_ap_count;
    s_last_scanning = model.wifi_scanning;
    strlcpy(s_last_status, model.wifi_status, sizeof(s_last_status));

    uint32_t child_count = lv_obj_get_child_count(s_list);
    for (int32_t i = (int32_t)child_count - 1; i >= 0; i--) {
        lv_obj_delete(lv_obj_get_child(s_list, i));
    }

    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    if (model.wifi_ap_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, model.wifi_scanning ? "扫描中..." : "暂无扫描结果");
        lv_obj_set_style_text_color(empty, theme->muted, 0);
        return;
    }

    static char ssid_cache[FRIDGE_UI_MAX_WIFI_APS][33];
    for (size_t i = 0; i < model.wifi_ap_count; i++) {
        strlcpy(ssid_cache[i], model.wifi_aps[i].ssid, sizeof(ssid_cache[i]));
        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_set_size(row, 610, 58);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, theme->surface, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, ap_cb, LV_EVENT_CLICKED, ssid_cache[i]);
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text_fmt(label, "%s  %ddBm  %s",
                              model.wifi_aps[i].ssid,
                              model.wifi_aps[i].rssi,
                              model.wifi_aps[i].secured ? "加密" : "开放");
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);
    }
}
