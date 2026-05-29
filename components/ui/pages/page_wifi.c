// 冰箱小精灵 Wi-Fi 配网页。
// 扫描在后台任务执行，页面只展示结果；选择热点后打开密码键盘，连接结果写回状态文案。

#include "fridge_ui_internal.h"

#include <stdint.h>
#include <string.h>

static lv_obj_t *s_status;
static lv_obj_t *s_list;
static char s_last_status[96];
static char s_last_connected_ssid[33];
static char s_last_connected_ip[16];
static size_t s_last_ap_count = SIZE_MAX;
static bool s_last_network_connected;
static bool s_last_scanning;

typedef struct {
    char ssid[33];
    bool secured;
} wifi_row_ctx_t;

static wifi_row_ctx_t s_row_ctx[FRIDGE_UI_MAX_WIFI_APS];

static const char *auth_text(const fridge_ui_wifi_ap_model_t *ap)
{
    if (!ap || !ap->secured) {
        return "OPEN";
    }
    return ap->authmode[0] ? ap->authmode : "SECURE";
}

static void style_card(lv_obj_t *obj)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_style_bg_color(obj, theme->surface, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_80, 0);
    lv_obj_set_style_border_color(obj, theme->line, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 18, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

static void create_connected_badge(lv_obj_t *parent, lv_obj_t *anchor)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *badge = lv_label_create(parent);
    lv_label_set_text(badge, "已连接");
    lv_obj_set_style_text_color(badge, lv_color_white(), 0);
    lv_obj_set_style_text_font(badge, fridge_ui_font_small(), 0);
    lv_obj_set_style_bg_color(badge, theme->accent, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(badge, 12, 0);
    lv_obj_set_style_pad_right(badge, 12, 0);
    lv_obj_set_style_pad_top(badge, 5, 0);
    lv_obj_set_style_pad_bottom(badge, 5, 0);
    lv_obj_set_style_radius(badge, 12, 0);
    lv_obj_align_to(badge, anchor, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
}

static void refresh_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_model_start_wifi_scan();
}

static void ap_cb(lv_event_t *event)
{
    wifi_row_ctx_t *row = lv_event_get_user_data(event);
    if (!row || row->ssid[0] == '\0') {
        return;
    }
    if (row->secured) {
        fridge_ui_keyboard_open("Wi-Fi", row->ssid);
    } else {
        fridge_ui_model_connect_wifi_async(row->ssid, "");
    }
}

void fridge_ui_page_wifi_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "设备网络");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "选择 Wi-Fi");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    lv_obj_t *refresh = lv_button_create(parent);
    lv_obj_set_size(refresh, 138, 52);
    lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, -36, 24);
    lv_obj_set_style_bg_color(refresh, theme->accent, 0);
    lv_obj_set_style_radius(refresh, 16, 0);
    lv_obj_set_style_shadow_width(refresh, 0, 0);
    lv_obj_add_event_cb(refresh, refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh_label = lv_label_create(refresh);
    lv_label_set_text(refresh_label, "刷新");
    lv_obj_center(refresh_label);

    s_status = lv_label_create(parent);
    lv_obj_set_style_text_color(s_status, theme->muted, 0);
    lv_obj_set_style_text_font(s_status, fridge_ui_font_small(), 0);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_status, 648);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 36, 82);

    s_list = lv_obj_create(parent);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_pos(s_list, 30, 116);
    lv_obj_set_size(s_list, 660, 392);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_bottom(s_list, 14, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_list, 10, 0);
}

void fridge_ui_page_wifi_update(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    fridge_ui_label_set_text_if_changed(s_status, model.wifi_status);

    if (s_last_ap_count == model.wifi_ap_count &&
        s_last_scanning == model.wifi_scanning &&
        s_last_network_connected == model.network.connected &&
        strcmp(s_last_status, model.wifi_status) == 0 &&
        strcmp(s_last_connected_ssid, model.network.ssid) == 0 &&
        strcmp(s_last_connected_ip, model.network.ip) == 0) {
        return;
    }
    s_last_ap_count = model.wifi_ap_count;
    s_last_scanning = model.wifi_scanning;
    s_last_network_connected = model.network.connected;
    strlcpy(s_last_status, model.wifi_status, sizeof(s_last_status));
    strlcpy(s_last_connected_ssid, model.network.ssid, sizeof(s_last_connected_ssid));
    strlcpy(s_last_connected_ip, model.network.ip, sizeof(s_last_connected_ip));

    uint32_t child_count = lv_obj_get_child_count(s_list);
    for (int32_t i = (int32_t)child_count - 1; i >= 0; i--) {
        lv_obj_delete(lv_obj_get_child(s_list, i));
    }

    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    bool has_current_network = model.network.connected && model.network.ssid[0] != '\0';
    bool current_in_scan_results = false;
    for (size_t i = 0; i < model.wifi_ap_count; i++) {
        if (has_current_network && strcmp(model.network.ssid, model.wifi_aps[i].ssid) == 0) {
            current_in_scan_results = true;
            break;
        }
    }

    if (has_current_network && !current_in_scan_results) {
        lv_obj_t *current = lv_obj_create(s_list);
        lv_obj_set_size(current, 624, 92);
        lv_obj_remove_flag(current, LV_OBJ_FLAG_SCROLLABLE);
        style_card(current);
        lv_obj_set_style_bg_color(current, theme->surface_soft, 0);
        lv_obj_set_style_border_color(current, theme->accent, 0);

        lv_obj_t *ssid = lv_label_create(current);
        lv_label_set_text(ssid, model.network.ssid);
        lv_obj_set_style_text_color(ssid, theme->text, 0);
        lv_obj_set_style_text_font(ssid, fridge_ui_font_body(), 0);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
        lv_obj_set_width(ssid, 300);
        lv_obj_align(ssid, LV_ALIGN_TOP_LEFT, 14, 14);
        create_connected_badge(current, ssid);

        lv_obj_t *meta = lv_label_create(current);
        lv_label_set_text_fmt(meta,
                              "当前网络  %ddBm  IP %s",
                              model.network.rssi,
                              model.network.ip[0] ? model.network.ip : "--");
        lv_obj_set_style_text_color(meta, theme->muted, 0);
        lv_obj_set_style_text_font(meta, fridge_ui_font_small(), 0);
        lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
        lv_obj_set_width(meta, 470);
        lv_obj_align(meta, LV_ALIGN_TOP_LEFT, 14, 52);
    }

    if (model.wifi_ap_count == 0) {
        lv_obj_t *empty = lv_obj_create(s_list);
        lv_obj_set_size(empty, 624, 126);
        lv_obj_remove_flag(empty, LV_OBJ_FLAG_SCROLLABLE);
        style_card(empty);
        lv_obj_t *empty_title = lv_label_create(empty);
        lv_label_set_text(empty_title, model.wifi_scanning ? "扫描中..." : "暂无扫描结果");
        lv_obj_set_style_text_font(empty_title, fridge_ui_font_body(), 0);
        lv_obj_align(empty_title, LV_ALIGN_TOP_MID, 0, 24);
        lv_obj_t *empty_hint = lv_label_create(empty);
        lv_label_set_text(empty_hint, model.wifi_scanning ? "请稍等，正在读取附近 2.4GHz 热点。" : "点击右上角刷新，扫描开发板附近的 Wi-Fi。");
        lv_obj_set_style_text_color(empty_hint, theme->muted, 0);
        lv_obj_set_style_text_font(empty_hint, fridge_ui_font_small(), 0);
        lv_label_set_long_mode(empty_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty_hint, 540);
        lv_obj_align(empty_hint, LV_ALIGN_TOP_MID, 0, 66);
        return;
    }

    for (size_t i = 0; i < model.wifi_ap_count; i++) {
        strlcpy(s_row_ctx[i].ssid, model.wifi_aps[i].ssid, sizeof(s_row_ctx[i].ssid));
        s_row_ctx[i].secured = model.wifi_aps[i].secured;
        bool connected = model.network.connected && strcmp(model.network.ssid, model.wifi_aps[i].ssid) == 0;

        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_set_size(row, 624, 82);
        style_card(row);
        lv_obj_add_event_cb(row, ap_cb, LV_EVENT_CLICKED, &s_row_ctx[i]);
        if (connected) {
            lv_obj_set_style_bg_color(row, theme->surface_soft, 0);
            lv_obj_set_style_border_color(row, theme->accent, 0);
        }

        lv_obj_t *ssid = lv_label_create(row);
        lv_label_set_text(ssid, model.wifi_aps[i].ssid);
        lv_obj_set_style_text_color(ssid, theme->text, 0);
        lv_obj_set_style_text_font(ssid, fridge_ui_font_body(), 0);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
        lv_obj_set_width(ssid, connected ? 270 : 350);
        lv_obj_align(ssid, LV_ALIGN_TOP_LEFT, 14, 12);

        if (connected) {
            create_connected_badge(row, ssid);
        }

        lv_obj_t *meta = lv_label_create(row);
        lv_label_set_text_fmt(meta, "%ddBm  %s",
                              model.wifi_aps[i].rssi,
                              auth_text(&model.wifi_aps[i]));
        lv_obj_set_style_text_color(meta, theme->muted, 0);
        lv_obj_set_style_text_font(meta, fridge_ui_font_small(), 0);
        lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
        lv_obj_set_width(meta, 360);
        lv_obj_align(meta, LV_ALIGN_TOP_LEFT, 14, 46);

        lv_obj_t *action = lv_label_create(row);
        lv_label_set_text(action, connected ? "重新连接" : "连接");
        lv_obj_set_style_text_color(action, connected ? theme->text : lv_color_white(), 0);
        lv_obj_set_style_text_font(action, fridge_ui_font_body(), 0);
        lv_obj_set_style_bg_color(action, connected ? theme->surface : theme->accent, 0);
        lv_obj_set_style_bg_opa(action, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(action, 18, 0);
        lv_obj_set_style_pad_right(action, 18, 0);
        lv_obj_set_style_pad_top(action, 10, 0);
        lv_obj_set_style_pad_bottom(action, 10, 0);
        lv_obj_set_style_radius(action, 16, 0);
        lv_obj_align(action, LV_ALIGN_RIGHT_MID, -14, 0);
    }
}
