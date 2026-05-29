// 冰箱小精灵设置页。
// 当前实现亮度滑块、Wi-Fi 入口和本地偏好开关；亮度会写 TR230S 寄存器并持久化到 NVS。

#include "fridge_ui_internal.h"

#include "fridge_display.h"

static lv_obj_t *s_brightness_label;
static lv_obj_t *s_slider;
static lv_obj_t *s_door_switch;
static lv_obj_t *s_wake_switch;
static uint8_t s_expire_days = 3;

static void slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    uint8_t value = (uint8_t)lv_slider_get_value(slider);
    fridge_display_set_brightness(value);
    fridge_ui_label_set_text_fmt_if_changed(s_brightness_label, "亮度 %u%%", value);
}

static void wifi_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_WIFI);
}

static void ops_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_toast("OTA / 日志后续接入");
}

static void expire_cb(lv_event_t *event)
{
    lv_obj_t *label = lv_event_get_user_data(event);
    s_expire_days = s_expire_days == 2 ? 3 : (s_expire_days == 3 ? 7 : 2);
    fridge_ui_label_set_text_fmt_if_changed(label, "临期提前提醒\n提前 %u 天", s_expire_days);
}

void fridge_ui_page_settings_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "设备与画面设置");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "小精灵偏好");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    s_brightness_label = lv_label_create(parent);
    lv_obj_align(s_brightness_label, LV_ALIGN_TOP_LEFT, 48, 104);

    s_slider = lv_slider_create(parent);
    lv_obj_set_size(s_slider, 560, 32);
    lv_obj_align(s_slider, LV_ALIGN_TOP_LEFT, 48, 142);
    lv_slider_set_range(s_slider, 45, 100);
    lv_slider_set_value(s_slider, fridge_display_get_brightness(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *wifi = lv_button_create(parent);
    lv_obj_set_size(wifi, 260, 70);
    lv_obj_align(wifi, LV_ALIGN_TOP_LEFT, 48, 196);
    lv_obj_set_style_bg_color(wifi, theme->accent, 0);
    lv_obj_set_style_radius(wifi, 8, 0);
    lv_obj_set_style_shadow_width(wifi, 0, 0);
    lv_obj_add_event_cb(wifi, wifi_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wifi_label = lv_label_create(wifi);
    lv_label_set_text(wifi_label, "Wi-Fi 配网");
    lv_obj_center(wifi_label);

    lv_obj_t *ops = lv_button_create(parent);
    lv_obj_set_size(ops, 260, 70);
    lv_obj_align(ops, LV_ALIGN_TOP_LEFT, 350, 196);
    lv_obj_set_style_bg_color(ops, theme->surface_soft, 0);
    lv_obj_set_style_radius(ops, 8, 0);
    lv_obj_set_style_shadow_width(ops, 0, 0);
    lv_obj_add_event_cb(ops, ops_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ops_label = lv_label_create(ops);
    lv_label_set_text(ops_label, "设备运维\nOTA / 日志");
    lv_obj_set_style_text_align(ops_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ops_label);

    lv_obj_t *door_label = lv_label_create(parent);
    lv_label_set_text(door_label, "开门提醒");
    lv_obj_align(door_label, LV_ALIGN_TOP_LEFT, 58, 302);
    s_door_switch = lv_switch_create(parent);
    lv_obj_align(s_door_switch, LV_ALIGN_TOP_RIGHT, -76, 290);
    lv_obj_add_state(s_door_switch, LV_STATE_CHECKED);

    lv_obj_t *wake_label = lv_label_create(parent);
    lv_label_set_text(wake_label, "语音唤醒");
    lv_obj_align(wake_label, LV_ALIGN_TOP_LEFT, 58, 362);
    s_wake_switch = lv_switch_create(parent);
    lv_obj_align(s_wake_switch, LV_ALIGN_TOP_RIGHT, -76, 350);
    lv_obj_add_state(s_wake_switch, LV_STATE_CHECKED);

    lv_obj_t *expire_btn = lv_button_create(parent);
    lv_obj_set_size(expire_btn, 560, 70);
    lv_obj_align(expire_btn, LV_ALIGN_TOP_LEFT, 58, 420);
    lv_obj_set_style_bg_color(expire_btn, theme->surface, 0);
    lv_obj_set_style_border_color(expire_btn, theme->line, 0);
    lv_obj_set_style_border_width(expire_btn, 1, 0);
    lv_obj_set_style_shadow_width(expire_btn, 0, 0);
    lv_obj_t *expire = lv_label_create(expire_btn);
    lv_label_set_text(expire, "临期提前提醒\n提前 3 天");
    lv_obj_center(expire);
    lv_obj_add_event_cb(expire_btn, expire_cb, LV_EVENT_CLICKED, expire);
}

void fridge_ui_page_settings_update(void)
{
    if (!s_brightness_label) {
        return;
    }
    uint8_t brightness = fridge_display_get_brightness();
    fridge_ui_label_set_text_fmt_if_changed(s_brightness_label, "亮度 %u%%", brightness);
    if (s_slider && lv_slider_get_value(s_slider) != brightness) {
        lv_slider_set_value(s_slider, brightness, LV_ANIM_OFF);
    }
}
