// 冰箱小精灵定时器页。
// 页面只读取厨房工具快照并发送本地控制命令；实际计时和到点播报在 kitchen_tools 后台任务中完成。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <stdint.h>

#include "fridge_kitchen_tools.h"

static lv_obj_t *s_time_label;
static lv_obj_t *s_state_label;
static lv_obj_t *s_label;
static lv_obj_t *s_alert;
static lv_obj_t *s_picker_label;
static uint32_t s_selected_seconds = 8 * 60;

static void format_duration(uint32_t seconds, char *out, size_t out_size)
{
    uint32_t minutes = seconds / 60;
    uint32_t remain = seconds % 60;
    snprintf(out, out_size, "%02lu:%02lu", (unsigned long)minutes, (unsigned long)remain);
}

static lv_obj_t *make_action(lv_obj_t *parent,
                             const char *text,
                             int16_t x,
                             int16_t y,
                             int16_t w,
                             lv_color_t color,
                             lv_event_cb_t cb,
                             void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, 54);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_body(), 0);
    lv_obj_center(label);
    return btn;
}

static void update_picker_label(void)
{
    if (!s_picker_label) {
        return;
    }
    char text[24] = {0};
    snprintf(text, sizeof(text), "%lu 分钟", (unsigned long)(s_selected_seconds / 60));
    fridge_ui_label_set_text_if_changed(s_picker_label, text);
}

static void adjust_minutes_cb(lv_event_t *event)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    int minutes = (int)(s_selected_seconds / 60) + delta;
    if (minutes < 1) {
        minutes = 1;
    } else if (minutes > 99) {
        minutes = 99;
    }
    s_selected_seconds = (uint32_t)minutes * 60;
    update_picker_label();
    fridge_ui_page_timer_update();
}

static void start_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = fridge_kitchen_tools_timer_start(s_selected_seconds, "厨房计时");
    if (err == ESP_OK) {
        fridge_ui_label_set_text_fmt_if_changed(s_alert, "已启动 %lu 分钟定时器", (unsigned long)(s_selected_seconds / 60));
    }
    fridge_ui_toast(err == ESP_OK ? "定时器已启动" : "定时器启动失败");
    fridge_ui_page_timer_update();
}

static void pause_resume_cb(lv_event_t *event)
{
    (void)event;
    fridge_kitchen_tools_snapshot_t snap = {0};
    if (fridge_kitchen_tools_get_snapshot(&snap) != ESP_OK) {
        fridge_ui_toast("读取定时器失败");
        return;
    }
    esp_err_t err = snap.timer.state == FRIDGE_KITCHEN_TIMER_PAUSED ? fridge_kitchen_tools_timer_resume()
                                                                     : fridge_kitchen_tools_timer_pause();
    fridge_ui_toast(err == ESP_OK ? "定时器状态已更新" : "当前不能暂停/继续");
    fridge_ui_page_timer_update();
}

static void cancel_cb(lv_event_t *event)
{
    (void)event;
    (void)fridge_kitchen_tools_timer_cancel();
    fridge_ui_toast("定时器已取消");
    fridge_ui_page_timer_update();
}

void fridge_ui_page_timer_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "厨房工具");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_ai_body(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "定时器");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(card, 660, 250);
    lv_obj_set_pos(card, 30, 96);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFF4D8), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xF0D49A), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);

    s_label = lv_label_create(card);
    lv_label_set_text(s_label, "厨房计时");
    lv_obj_set_style_text_color(s_label, theme->accent, 0);
    lv_obj_set_style_text_font(s_label, fridge_ui_font_body(), 0);
    lv_obj_align(s_label, LV_ALIGN_TOP_LEFT, 28, 24);

    s_time_label = lv_label_create(card);
    lv_label_set_text(s_time_label, "08:00");
    lv_obj_set_width(s_time_label, 460);
    lv_obj_set_style_text_color(s_time_label, theme->text, 0);
    lv_obj_set_style_text_font(s_time_label, fridge_ui_font_number(), 0);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -2);

    s_state_label = lv_label_create(card);
    lv_label_set_text(s_state_label, "可在下方调整时长");
    lv_obj_set_style_text_color(s_state_label, theme->muted, 0);
    lv_obj_set_style_text_font(s_state_label, fridge_ui_font_ai_body(), 0);
    lv_obj_align(s_state_label, LV_ALIGN_BOTTOM_MID, 0, -24);

    lv_obj_t *picker_title = lv_label_create(parent);
    lv_label_set_text(picker_title, "设置时长");
    lv_obj_set_style_text_color(picker_title, theme->muted, 0);
    lv_obj_set_style_text_font(picker_title, fridge_ui_font_ai_body(), 0);
    lv_obj_set_pos(picker_title, 48, 366);

    s_picker_label = lv_label_create(parent);
    lv_label_set_text(s_picker_label, "8 分钟");
    lv_obj_set_style_text_color(s_picker_label, theme->text, 0);
    lv_obj_set_style_text_font(s_picker_label, fridge_ui_font_body(), 0);
    lv_obj_set_pos(s_picker_label, 174, 358);
    lv_obj_set_width(s_picker_label, 130);

    make_action(parent, "-5", 312, 352, 76, lv_color_hex(0x8C7A43), adjust_minutes_cb, (void *)(intptr_t)-5);

    s_alert = lv_label_create(parent);
    lv_label_set_text(s_alert, "");
    lv_obj_set_width(s_alert, 640);
    lv_obj_set_style_text_color(s_alert, theme->accent_2, 0);
    lv_obj_set_style_text_font(s_alert, fridge_ui_font_ai_body(), 0);
    lv_obj_set_pos(s_alert, 42, 404);

    make_action(parent, "-1", 398, 352, 76, lv_color_hex(0x8C7A43), adjust_minutes_cb, (void *)(intptr_t)-1);
    make_action(parent, "+1", 484, 352, 76, theme->accent, adjust_minutes_cb, (void *)(intptr_t)1);
    make_action(parent, "+5", 570, 352, 76, theme->accent, adjust_minutes_cb, (void *)(intptr_t)5);

    make_action(parent, "开始", 62, 444, 184, theme->accent_2, start_cb, NULL);
    make_action(parent, "暂停/继续", 268, 444, 184, theme->accent, pause_resume_cb, NULL);
    make_action(parent, "取消", 474, 444, 184, lv_color_hex(0x8C7A43), cancel_cb, NULL);
    update_picker_label();
}

void fridge_ui_page_timer_update(void)
{
    if (!s_time_label) {
        return;
    }
    fridge_kitchen_tools_snapshot_t snap = {0};
    if (fridge_kitchen_tools_get_snapshot(&snap) != ESP_OK) {
        return;
    }
    char time_text[16] = {0};
    format_duration(snap.timer.state == FRIDGE_KITCHEN_TIMER_IDLE ? s_selected_seconds : snap.timer.remaining_seconds,
                    time_text,
                    sizeof(time_text));
    fridge_ui_label_set_text_if_changed(s_time_label, time_text);
    fridge_ui_label_set_text_if_changed(s_label, snap.timer.label[0] ? snap.timer.label : "厨房计时");
    const char *state = "可在下方调整时长";
    if (snap.timer.state == FRIDGE_KITCHEN_TIMER_RUNNING) {
        state = "正在倒计时";
    } else if (snap.timer.state == FRIDGE_KITCHEN_TIMER_PAUSED) {
        state = "已暂停";
    } else if (snap.timer.state == FRIDGE_KITCHEN_TIMER_RINGING) {
        state = "时间到了";
    }
    fridge_ui_label_set_text_if_changed(s_state_label, state);
    fridge_ui_label_set_text_if_changed(s_alert, snap.last_alert);
}
