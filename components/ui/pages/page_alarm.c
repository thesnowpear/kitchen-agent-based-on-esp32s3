// 冰箱小精灵闹钟页。
// 闹钟依赖 SNTP 后的系统时间；未校时时只展示风险提示，不允许创建新的绝对时间闹钟。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <stdint.h>

#include "fridge_kitchen_tools.h"

static lv_obj_t *s_status;
static lv_obj_t *s_picker_label;
static lv_obj_t *s_alarm_rows[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
static lv_obj_t *s_alarm_labels[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
static lv_obj_t *s_cancel_label;
static uint8_t s_alarm_ids[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
static uint8_t s_selected_alarm_id;
static uint8_t s_alarm_hour = 7;
static uint8_t s_alarm_minute = 0;

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
    fridge_ui_label_set_text_fmt_if_changed(s_picker_label, "%02u:%02u", s_alarm_hour, s_alarm_minute);
}

static void adjust_alarm_cb(lv_event_t *event)
{
    int code = (int)(intptr_t)lv_event_get_user_data(event);
    if (code == 60) {
        s_alarm_hour = (uint8_t)((s_alarm_hour + 1) % 24);
    } else if (code == -60) {
        s_alarm_hour = (uint8_t)((s_alarm_hour + 23) % 24);
    } else if (code == 1) {
        s_alarm_minute = (uint8_t)((s_alarm_minute + 5) % 60);
    } else if (code == -1) {
        s_alarm_minute = (uint8_t)((s_alarm_minute + 55) % 60);
    }
    update_picker_label();
}

static void add_alarm_cb(lv_event_t *event)
{
    (void)event;
    uint8_t id = 0;
    esp_err_t err = fridge_kitchen_tools_alarm_set(s_alarm_hour, s_alarm_minute, "厨房提醒", &id);
    if (err == ESP_ERR_INVALID_STATE) {
        fridge_ui_toast("时间未同步，先连接 Wi-Fi");
    } else {
        fridge_ui_toast(err == ESP_OK ? "闹钟已添加" : "闹钟设置失败");
    }
    fridge_ui_page_alarm_update();
}

static void select_alarm_cb(lv_event_t *event)
{
    uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= FRIDGE_KITCHEN_TOOL_MAX_ALARMS || s_alarm_ids[index] == 0) {
        fridge_ui_toast("暂无可选闹钟");
        return;
    }
    s_selected_alarm_id = s_alarm_ids[index];
    fridge_ui_page_alarm_update();
}

static void cancel_selected_cb(lv_event_t *event)
{
    (void)event;
    if (s_selected_alarm_id == 0) {
        fridge_ui_toast("请先点选一个闹钟");
        return;
    }
    esp_err_t err = fridge_kitchen_tools_alarm_cancel(s_selected_alarm_id);
    fridge_ui_toast(err == ESP_OK ? "已取消闹钟" : "闹钟取消失败");
    if (err == ESP_OK) {
        s_selected_alarm_id = 0;
    }
    fridge_ui_page_alarm_update();
}

void fridge_ui_page_alarm_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "厨房工具");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_ai_body(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "闹钟");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    s_status = lv_label_create(parent);
    lv_label_set_text(s_status, "同步时间后可设置固定时间提醒");
    lv_obj_set_style_text_color(s_status, theme->muted, 0);
    lv_obj_set_style_text_font(s_status, fridge_ui_font_ai_body(), 0);
    lv_obj_set_pos(s_status, 38, 84);
    lv_obj_set_width(s_status, 640);

    s_picker_label = lv_label_create(parent);
    lv_label_set_text(s_picker_label, "07:00");
    lv_obj_set_style_text_color(s_picker_label, theme->text, 0);
    lv_obj_set_style_text_font(s_picker_label, fridge_ui_font_number(), 0);
    lv_obj_set_width(s_picker_label, 420);
    lv_obj_set_style_text_align(s_picker_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_picker_label, 150, 138);

    make_action(parent, "时-", 60, 124, 112, lv_color_hex(0x8C7A43), adjust_alarm_cb, (void *)(intptr_t)-60);
    make_action(parent, "时+", 548, 124, 112, theme->accent, adjust_alarm_cb, (void *)(intptr_t)60);
    make_action(parent, "分-", 60, 184, 112, lv_color_hex(0x8C7A43), adjust_alarm_cb, (void *)(intptr_t)-1);
    make_action(parent, "分+", 548, 184, 112, theme->accent, adjust_alarm_cb, (void *)(intptr_t)1);

    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 30, 248);
    lv_obj_set_size(list, 660, 178);
    lv_obj_set_style_bg_color(list, lv_color_hex(0xFFF0F1), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(list, lv_color_hex(0xE6C8CC), 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_radius(list, 24, 0);

    for (uint8_t i = 0; i < FRIDGE_KITCHEN_TOOL_MAX_ALARMS; i++) {
        // 每一行都是可点选的闹钟项；取消按钮删除当前选中项，避免只能删第一条。
        s_alarm_rows[i] = lv_button_create(list);
        lv_obj_remove_style_all(s_alarm_rows[i]);
        lv_obj_set_pos(s_alarm_rows[i], 16, 10 + i * 32);
        lv_obj_set_size(s_alarm_rows[i], 628, 30);
        lv_obj_set_style_radius(s_alarm_rows[i], 12, 0);
        lv_obj_set_style_bg_opa(s_alarm_rows[i], LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(s_alarm_rows[i], select_alarm_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        s_alarm_labels[i] = lv_label_create(s_alarm_rows[i]);
        lv_obj_set_pos(s_alarm_labels[i], 12, 1);
        lv_obj_set_width(s_alarm_labels[i], 604);
        lv_obj_set_style_text_color(s_alarm_labels[i], theme->text, 0);
        lv_obj_set_style_text_font(s_alarm_labels[i], fridge_ui_font_body(), 0);
        lv_label_set_long_mode(s_alarm_labels[i], LV_LABEL_LONG_DOT);
    }

    make_action(parent, "添加闹钟", 62, 448, 286, theme->accent, add_alarm_cb, NULL);
    lv_obj_t *cancel_btn = make_action(parent, "删除闹钟", 372, 448, 286, theme->accent_2, cancel_selected_cb, NULL);
    s_cancel_label = lv_obj_get_child(cancel_btn, 0);
    update_picker_label();
}

void fridge_ui_page_alarm_update(void)
{
    if (!s_status) {
        return;
    }
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    fridge_kitchen_tools_snapshot_t snap = {0};
    if (fridge_kitchen_tools_get_snapshot(&snap) != ESP_OK) {
        return;
    }
    fridge_ui_label_set_text_if_changed(s_status, snap.time_ready ? "时间已同步，可在页面或通过 AI 创建闹钟" : "时间未同步：请先连接 Wi-Fi/SNTP");
    bool selected_exists = false;
    for (uint8_t i = 0; i < FRIDGE_KITCHEN_TOOL_MAX_ALARMS; i++) {
        s_alarm_ids[i] = i < snap.alarm_count ? snap.alarms[i].id : 0;
        if (s_alarm_ids[i] != 0 && s_alarm_ids[i] == s_selected_alarm_id) {
            selected_exists = true;
        }
    }
    if (!selected_exists) {
        s_selected_alarm_id = snap.alarm_count > 0 ? snap.alarms[0].id : 0;
    }
    if (s_cancel_label) {
        fridge_ui_label_set_text_if_changed(s_cancel_label, s_selected_alarm_id ? "删除闹钟" : "无可删除");
    }
    for (uint8_t i = 0; i < FRIDGE_KITCHEN_TOOL_MAX_ALARMS; i++) {
        if (!s_alarm_labels[i]) {
            continue;
        }
        if (i < snap.alarm_count) {
            const fridge_kitchen_alarm_t *alarm = &snap.alarms[i];
            bool selected = alarm->id == s_selected_alarm_id;
            char line[96];
            snprintf(line,
                     sizeof(line),
                     "%s%02u:%02u  %s%s",
                     selected ? "> " : "  ",
                     alarm->hour,
                     alarm->minute,
                     alarm->label[0] ? alarm->label : "厨房提醒",
                     alarm->ringing ? "  提醒中" : "");
            if (s_alarm_rows[i]) {
                lv_obj_set_style_bg_color(s_alarm_rows[i], selected ? lv_color_hex(0xF7D6D0) : lv_color_hex(0xFFF0F1), 0);
                lv_obj_set_style_bg_opa(s_alarm_rows[i], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            }
            lv_obj_set_style_text_color(s_alarm_labels[i], selected ? theme->accent_2 : theme->text, 0);
            fridge_ui_label_set_text_if_changed(s_alarm_labels[i], line);
        } else if (i == 0) {
            if (s_alarm_rows[i]) {
                lv_obj_set_style_bg_opa(s_alarm_rows[i], LV_OPA_TRANSP, 0);
            }
            lv_obj_set_style_text_color(s_alarm_labels[i], theme->muted, 0);
            fridge_ui_label_set_text_if_changed(s_alarm_labels[i], "暂无闹钟");
        } else {
            if (s_alarm_rows[i]) {
                lv_obj_set_style_bg_opa(s_alarm_rows[i], LV_OPA_TRANSP, 0);
            }
            fridge_ui_label_set_text_if_changed(s_alarm_labels[i], "");
        }
    }
}
