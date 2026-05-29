// 冰箱小精灵秒表页。
// 秒表只保留本次运行状态，不写 Flash；刷新时从 kitchen_tools 快照换算显示，避免 UI tick 抖动影响计时。

#include "fridge_ui_internal.h"

#include <stdio.h>

#include "fridge_kitchen_tools.h"

static lv_obj_t *s_time_label;
static lv_obj_t *s_state_label;

static void format_elapsed(uint32_t seconds, char *out, size_t out_size)
{
    uint32_t hours = seconds / 3600;
    uint32_t minutes = (seconds / 60) % 60;
    uint32_t remain = seconds % 60;
    snprintf(out, out_size, "%02lu:%02lu:%02lu", (unsigned long)hours, (unsigned long)minutes, (unsigned long)remain);
}

static lv_obj_t *make_action(lv_obj_t *parent, const char *text, int16_t x, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 184, 58);
    lv_obj_set_pos(btn, x, 404);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_body(), 0);
    lv_obj_center(label);
    return btn;
}

static void start_cb(lv_event_t *event)
{
    (void)event;
    (void)fridge_kitchen_tools_stopwatch_start();
    fridge_ui_page_stopwatch_update();
}

static void pause_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = fridge_kitchen_tools_stopwatch_pause();
    fridge_ui_toast(err == ESP_OK ? "秒表已暂停" : "秒表未在运行");
    fridge_ui_page_stopwatch_update();
}

static void reset_cb(lv_event_t *event)
{
    (void)event;
    (void)fridge_kitchen_tools_stopwatch_reset();
    fridge_ui_toast("秒表已重置");
    fridge_ui_page_stopwatch_update();
}

void fridge_ui_page_stopwatch_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "厨房工具");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_ai_body(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "秒表");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(card, 660, 286);
    lv_obj_set_pos(card, 30, 104);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF0EEFF), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xD8D2F3), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, "烹饪计时");
    lv_obj_set_style_text_color(label, theme->accent, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_body(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 28, 24);

    s_time_label = lv_label_create(card);
    lv_label_set_text(s_time_label, "00:00:00");
    lv_obj_set_width(s_time_label, 620);
    lv_obj_set_style_text_color(s_time_label, theme->text, 0);
    lv_obj_set_style_text_font(s_time_label, fridge_ui_font_number(), 0);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -2);

    s_state_label = lv_label_create(card);
    lv_label_set_text(s_state_label, "适合揉面、醒发和煎烤计时");
    lv_obj_set_style_text_color(s_state_label, theme->muted, 0);
    lv_obj_set_style_text_font(s_state_label, fridge_ui_font_ai_body(), 0);
    lv_obj_align(s_state_label, LV_ALIGN_BOTTOM_MID, 0, -34);

    make_action(parent, "开始", 62, theme->accent_2, start_cb);
    make_action(parent, "暂停", 268, theme->accent, pause_cb);
    make_action(parent, "重置", 474, lv_color_hex(0x706AB8), reset_cb);
}

void fridge_ui_page_stopwatch_update(void)
{
    if (!s_time_label) {
        return;
    }
    fridge_kitchen_tools_snapshot_t snap = {0};
    if (fridge_kitchen_tools_get_snapshot(&snap) != ESP_OK) {
        return;
    }
    char text[16] = {0};
    format_elapsed(snap.stopwatch.elapsed_seconds, text, sizeof(text));
    fridge_ui_label_set_text_if_changed(s_time_label, text);
    const char *state = "适合揉面、醒发和煎烤计时";
    if (snap.stopwatch.state == FRIDGE_KITCHEN_STOPWATCH_RUNNING) {
        state = "正在计时";
    } else if (snap.stopwatch.state == FRIDGE_KITCHEN_STOPWATCH_PAUSED) {
        state = "已暂停";
    }
    fridge_ui_label_set_text_if_changed(s_state_label, state);
}
