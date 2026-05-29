// 冰箱小精灵设置页。
// 当前实现亮度滑块、Wi-Fi 入口和本地偏好开关；亮度会写 TR230S 寄存器并持久化到 NVS。

#include "fridge_ui_internal.h"

#include "fridge_display.h"
#include "fridge_speaker.h"
#include "fridge_state_machine.h"

static lv_obj_t *s_brightness_label;
static lv_obj_t *s_brightness_slider;
static lv_obj_t *s_volume_label;
static lv_obj_t *s_volume_slider;
static lv_obj_t *s_tts_switch;
static lv_obj_t *s_sleep_switch;
static lv_obj_t *s_door_switch;
static lv_obj_t *s_wake_switch;
static lv_obj_t *s_expire_label;
static uint8_t s_expire_days = 3;

static void style_card(lv_obj_t *obj, int32_t radius)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_style_bg_color(obj, theme->surface, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_80, 0);
    lv_obj_set_style_border_color(obj, theme->line, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

static lv_obj_t *create_row(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 624, height);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    style_card(row, 18);
    lv_obj_set_style_pad_all(row, 14, 0);
    return row;
}

static lv_obj_t *create_button_row(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, 624, height);
    style_card(row, 18);
    lv_obj_set_style_pad_all(row, 14, 0);
    return row;
}

static void brightness_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    uint8_t value = (uint8_t)lv_slider_get_value(slider);
    fridge_display_set_brightness(value);
    fridge_ui_label_set_text_fmt_if_changed(s_brightness_label, "亮度 %u%%", value);
}

static void volume_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    uint8_t value = (uint8_t)lv_slider_get_value(slider);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        (void)fridge_speaker_preview_volume(value);
    } else {
        (void)fridge_speaker_set_volume(value);
    }
    fridge_ui_label_set_text_fmt_if_changed(s_volume_label, "音量 %u%%", value);
}

static void tts_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    (void)fridge_speaker_set_tts_enabled(enabled);
    fridge_ui_toast(enabled ? "TTS 自动播报已开启" : "TTS 自动播报已关闭");
}

static void sleep_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    fridge_sm_config_t config = {0};
    if (fridge_state_machine_get_config(&config) != ESP_OK) {
        fridge_ui_toast("状态机配置读取失败");
        return;
    }
    config.sleep_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    esp_err_t err = fridge_state_machine_set_config(&config);
    fridge_ui_toast(err == ESP_OK ? (config.sleep_enabled ? "黑屏休眠已开启" : "黑屏休眠已关闭") : "休眠设置保存失败");
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
    fridge_ui_label_set_text_fmt_if_changed(label, "提前 %u 天", s_expire_days);
}

void fridge_ui_page_settings_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "设备与画面设置");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "小精灵偏好");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 30, 92);
    lv_obj_set_size(list, 660, 430);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_bottom(list, 16, 0);

    // 亮度行按参考页保留滑块和百分比，实际写入 TR230S 背光寄存器。
    lv_obj_t *brightness = create_row(list, 92);
    lv_obj_align(brightness, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *brightness_title = lv_label_create(brightness);
    lv_label_set_text(brightness_title, "屏幕亮度");
    lv_obj_set_style_text_font(brightness_title, fridge_ui_font_body(), 0);
    lv_obj_align(brightness_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_brightness_label = lv_label_create(brightness);
    lv_obj_set_style_text_color(s_brightness_label, theme->muted, 0);
    lv_obj_set_style_text_font(s_brightness_label, fridge_ui_font_small(), 0);
    lv_obj_align(s_brightness_label, LV_ALIGN_TOP_RIGHT, 0, 2);

    s_brightness_slider = lv_slider_create(brightness);
    lv_obj_set_size(s_brightness_slider, 584, 24);
    lv_obj_align(s_brightness_slider, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_slider_set_range(s_brightness_slider, 45, 100);
    lv_slider_set_value(s_brightness_slider, fridge_display_get_brightness(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_brightness_slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // 扬声器音量通过 PCM 数字缩放实现，保存到 TTS 配置 NVS，避免改动 MAX98357A 硬件增益脚。
    lv_obj_t *volume = create_row(list, 92);
    lv_obj_align(volume, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_t *volume_title = lv_label_create(volume);
    lv_label_set_text(volume_title, "扬声器音量");
    lv_obj_set_style_text_font(volume_title, fridge_ui_font_body(), 0);
    lv_obj_align(volume_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_volume_label = lv_label_create(volume);
    lv_obj_set_style_text_color(s_volume_label, theme->muted, 0);
    lv_obj_set_style_text_font(s_volume_label, fridge_ui_font_small(), 0);
    lv_obj_align(s_volume_label, LV_ALIGN_TOP_RIGHT, 0, 2);

    s_volume_slider = lv_slider_create(volume);
    lv_obj_set_size(s_volume_slider, 584, 24);
    lv_obj_align(s_volume_slider, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_slider_set_range(s_volume_slider, 0, 100);
    lv_slider_set_value(s_volume_slider, fridge_speaker_get_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_volume_slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_volume_slider, volume_slider_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *tts = create_row(list, 62);
    lv_obj_align(tts, LV_ALIGN_TOP_MID, 0, 208);
    lv_obj_t *tts_label = lv_label_create(tts);
    lv_label_set_text(tts_label, "自动语音播报");
    lv_obj_align(tts_label, LV_ALIGN_LEFT_MID, 0, 0);
    s_tts_switch = lv_switch_create(tts);
    lv_obj_align(s_tts_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    if (fridge_speaker_get_tts_enabled()) {
        lv_obj_add_state(s_tts_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_tts_switch, tts_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // 休眠开关用于传感器未全部接入时联调：关闭后最低停在待机颜文字页，不把背光压到 0。
    lv_obj_t *sleep = create_row(list, 62);
    lv_obj_align(sleep, LV_ALIGN_TOP_MID, 0, 282);
    lv_obj_t *sleep_label = lv_label_create(sleep);
    lv_label_set_text(sleep_label, "黑屏休眠");
    lv_obj_align(sleep_label, LV_ALIGN_LEFT_MID, 0, 0);
    s_sleep_switch = lv_switch_create(sleep);
    lv_obj_align(s_sleep_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    fridge_sm_config_t sm_config = {0};
    if (fridge_state_machine_get_config(&sm_config) == ESP_OK && sm_config.sleep_enabled) {
        lv_obj_add_state(s_sleep_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_sleep_switch, sleep_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Wi-Fi 设置作为独立页面进入，避免扫描列表挤压设置页内容。
    lv_obj_t *wifi = create_button_row(list, 72);
    lv_obj_align(wifi, LV_ALIGN_TOP_MID, 0, 354);
    lv_obj_set_style_bg_color(wifi, theme->accent, 0);
    lv_obj_set_style_bg_opa(wifi, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(wifi, wifi_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wifi_title = lv_label_create(wifi);
    lv_label_set_text(wifi_title, "Wi-Fi 网络");
    lv_obj_set_style_text_color(wifi_title, lv_color_white(), 0);
    lv_obj_align(wifi_title, LV_ALIGN_LEFT_MID, 0, -12);
    lv_obj_t *wifi_label = lv_label_create(wifi);
    lv_label_set_text(wifi_label, "查看可用网络");
    lv_obj_set_style_text_color(wifi_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(wifi_label, fridge_ui_font_small(), 0);
    lv_obj_align(wifi_label, LV_ALIGN_LEFT_MID, 0, 16);
    lv_obj_t *wifi_arrow = lv_label_create(wifi);
    lv_label_set_text(wifi_arrow, ">");
    lv_obj_set_style_text_color(wifi_arrow, lv_color_white(), 0);
    lv_obj_align(wifi_arrow, LV_ALIGN_RIGHT_MID, -2, 0);

    lv_obj_t *ops = create_button_row(list, 72);
    lv_obj_align(ops, LV_ALIGN_TOP_MID, 0, 438);
    lv_obj_set_style_bg_color(ops, theme->surface_soft, 0);
    lv_obj_add_event_cb(ops, ops_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ops_title = lv_label_create(ops);
    lv_label_set_text(ops_title, "设备运维");
    lv_obj_set_style_text_color(ops_title, theme->text, 0);
    lv_obj_set_style_text_font(ops_title, fridge_ui_font_body(), 0);
    lv_obj_align(ops_title, LV_ALIGN_LEFT_MID, 0, -12);
    lv_obj_t *ops_label = lv_label_create(ops);
    lv_label_set_text(ops_label, "OTA / 日志");
    lv_obj_set_style_text_color(ops_label, theme->muted, 0);
    lv_obj_set_style_text_font(ops_label, fridge_ui_font_small(), 0);
    lv_obj_align(ops_label, LV_ALIGN_LEFT_MID, 0, 16);

    lv_obj_t *door = create_row(list, 62);
    lv_obj_align(door, LV_ALIGN_TOP_MID, 0, 522);
    lv_obj_t *door_label = lv_label_create(door);
    lv_label_set_text(door_label, "开门提醒");
    lv_obj_align(door_label, LV_ALIGN_LEFT_MID, 0, 0);
    s_door_switch = lv_switch_create(door);
    lv_obj_align(s_door_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_state(s_door_switch, LV_STATE_CHECKED);

    lv_obj_t *wake = create_row(list, 62);
    lv_obj_align(wake, LV_ALIGN_TOP_MID, 0, 594);
    lv_obj_t *wake_label = lv_label_create(wake);
    lv_label_set_text(wake_label, "语音唤醒");
    lv_obj_align(wake_label, LV_ALIGN_LEFT_MID, 0, 0);
    s_wake_switch = lv_switch_create(wake);
    lv_obj_align(s_wake_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_state(s_wake_switch, LV_STATE_CHECKED);

    lv_obj_t *expire_btn = create_button_row(list, 72);
    lv_obj_align(expire_btn, LV_ALIGN_TOP_MID, 0, 666);
    lv_obj_t *expire_title = lv_label_create(expire_btn);
    lv_label_set_text(expire_title, "临期提前提醒");
    lv_obj_set_style_text_color(expire_title, theme->text, 0);
    lv_obj_set_style_text_font(expire_title, fridge_ui_font_body(), 0);
    lv_obj_align(expire_title, LV_ALIGN_LEFT_MID, 0, -12);
    s_expire_label = lv_label_create(expire_btn);
    lv_label_set_text(s_expire_label, "提前 3 天");
    lv_obj_set_style_text_color(s_expire_label, theme->muted, 0);
    lv_obj_set_style_text_font(s_expire_label, fridge_ui_font_small(), 0);
    lv_obj_align(s_expire_label, LV_ALIGN_LEFT_MID, 0, 16);
    lv_obj_add_event_cb(expire_btn, expire_cb, LV_EVENT_CLICKED, s_expire_label);
}

void fridge_ui_page_settings_update(void)
{
    if (!s_brightness_label) {
        return;
    }
    uint8_t brightness = fridge_display_get_brightness();
    fridge_ui_label_set_text_fmt_if_changed(s_brightness_label, "亮度 %u%%", brightness);
    if (s_brightness_slider && lv_slider_get_value(s_brightness_slider) != brightness) {
        lv_slider_set_value(s_brightness_slider, brightness, LV_ANIM_OFF);
    }
    uint8_t volume = fridge_speaker_get_volume();
    fridge_ui_label_set_text_fmt_if_changed(s_volume_label, "音量 %u%%", volume);
    if (s_volume_slider && lv_slider_get_value(s_volume_slider) != volume) {
        lv_slider_set_value(s_volume_slider, volume, LV_ANIM_OFF);
    }
    if (s_tts_switch) {
        bool enabled = fridge_speaker_get_tts_enabled();
        if (enabled) {
            lv_obj_add_state(s_tts_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_tts_switch, LV_STATE_CHECKED);
        }
    }
    if (s_sleep_switch) {
        fridge_sm_config_t sm_config = {0};
        bool enabled = fridge_state_machine_get_config(&sm_config) == ESP_OK && sm_config.sleep_enabled;
        if (enabled) {
            lv_obj_add_state(s_sleep_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_sleep_switch, LV_STATE_CHECKED);
        }
    }
}
