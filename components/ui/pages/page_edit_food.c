// 冰箱小精灵食材编辑页。
// 页面参照 ui-reference 的 editFood 表单结构：两列字段、整栏位置编辑、备注宽行和语音/手动补充区。
// 实屏采用“标题在左、内容在右”的字段行，减少小屏纵向挤压。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "fridge_wake_word.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EDIT_FOOD_ASR_TASK_STACK 8192

static const char *TAG = "fridge_ui_edit_food";

typedef struct {
    lv_obj_t *button;
    lv_obj_t *title;
    lv_obj_t *value;
} edit_field_view_t;

static lv_obj_t *s_title;
static edit_field_view_t s_name;
static edit_field_view_t s_quantity;
static edit_field_view_t s_expire;
static edit_field_view_t s_location;
static edit_field_view_t s_stored;
static edit_field_view_t s_note;
static lv_obj_t *s_manual_preview;
static lv_obj_t *s_voice_status;
static lv_obj_t *s_scroll;
static lv_obj_t *s_top_actions;
static lv_obj_t *s_voice_sheet;
static lv_obj_t *s_voice_sheet_status;
static lv_obj_t *s_voice_sheet_hint;
static lv_obj_t *s_voice_sheet_button_label;
static char s_note_text[128];
static char s_stored_text[32];
static fridge_ui_food_t s_editing;
static uint8_t s_last_zone = UINT8_MAX;
static uint8_t s_last_cell = UINT8_MAX;
static bool s_voice_recording;
static bool s_voice_busy;
static bool s_wake_was_listening;
static uint32_t s_voice_generation;

static const char *CELL_DISPLAY_NAMES[FRIDGE_UI_ZONE_CELL_COUNT] = {"内 · 左", "内 · 中", "内 · 右",
                                                                     "中 · 左", "中 · 中", "中 · 右",
                                                                     "外 · 左", "外 · 中", "外 · 右"};

static const char *active_cell_display(uint8_t cell)
{
    return cell < FRIDGE_UI_ZONE_CELL_COUNT ? CELL_DISPLAY_NAMES[cell] : "中 · 中";
}

static void normalize_expire_days(void)
{
    int days = atoi(s_editing.expire);
    if (strstr(s_editing.expire, "今天")) {
        s_editing.days_left = 0;
    } else if (strstr(s_editing.expire, "明天")) {
        s_editing.days_left = 1;
    } else if ((unsigned char)s_editing.expire[0] >= '0' && (unsigned char)s_editing.expire[0] <= '9') {
        s_editing.days_left = days;
    }
}

static void make_default_stored_text(char *out, size_t out_size, uint8_t zone, uint8_t cell)
{
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "2026-05-%02u %02u:%02u",
             (unsigned)(21 + ((zone + cell) % 6)),
             (unsigned)(8 + ((zone * 3 + cell) % 12)),
             (unsigned)((cell * 7) % 60));
}

static void set_field(edit_field_view_t *field, const char *value)
{
    if (!field || !field->value) {
        return;
    }
    fridge_ui_label_set_text_if_changed(field->value, value && value[0] ? value : "未填写");
}

static void set_location_value(void)
{
    char place[64] = {0};
    const char *zone_name = "";
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    if (s_editing.zone < FRIDGE_UI_ZONE_COUNT) {
        zone_name = model.zones[s_editing.zone].name;
    }
    snprintf(place, sizeof(place), "%s %s", zone_name[0] ? zone_name : "冰箱区域", active_cell_display(s_editing.cell));
    strlcpy(s_editing.location, place, sizeof(s_editing.location));
    set_field(&s_location, place);
}

static const char *default_note_text(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return "";
    }
    if (s_editing.name[0] == '\0') {
        strlcpy(buf, "可登记新食材。", buf_size);
        return buf;
    }
    snprintf(buf, buf_size, "来自%s，%s。",
             s_editing.location[0] ? s_editing.location : "当前位置",
             s_editing.quantity[0] ? s_editing.quantity : "待确认");
    return buf;
}

static void refresh_fields(void)
{
    char note_buf[96] = {0};
    const char *note = s_note_text[0] ? s_note_text : default_note_text(note_buf, sizeof(note_buf));
    set_field(&s_name, s_editing.name);
    set_field(&s_quantity, s_editing.quantity);
    set_field(&s_expire, s_editing.expire);
    set_location_value();
    set_field(&s_stored, s_stored_text);
    set_field(&s_note, note);
    fridge_ui_label_set_text_if_changed(s_manual_preview, s_note_text[0] ? s_note_text : "未输入补充信息，可点击手动补充登记信息，或使用语音补充数量、位置和备注。");
}

static void edit_name_done(const char *text)
{
    strlcpy(s_editing.name, text ? text : "", sizeof(s_editing.name));
    refresh_fields();
}

static void edit_quantity_done(const char *text)
{
    strlcpy(s_editing.quantity, text ? text : "", sizeof(s_editing.quantity));
    refresh_fields();
}

static void edit_expire_done(const char *text)
{
    strlcpy(s_editing.expire, text ? text : "", sizeof(s_editing.expire));
    normalize_expire_days();
    refresh_fields();
}

static void edit_stored_done(const char *text)
{
    strlcpy(s_stored_text, text ? text : "", sizeof(s_stored_text));
    refresh_fields();
}

static void edit_note_done(const char *text)
{
    strlcpy(s_note_text, text ? text : "", sizeof(s_note_text));
    refresh_fields();
    fridge_ui_label_set_text_if_changed(s_voice_status, "已追加到备注。");
}

static void restore_wake_if_needed(void)
{
    if (s_wake_was_listening) {
        esp_err_t err = fridge_wake_word_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restart wake word failed: %s", esp_err_to_name(err));
        }
    }
    s_wake_was_listening = false;
}

static void set_voice_sheet_state(const char *button, const char *status, const char *hint)
{
    fridge_ui_label_set_text_if_changed(s_voice_sheet_button_label, button ? button : "开始录音");
    fridge_ui_label_set_text_if_changed(s_voice_sheet_status, status ? status : "");
    fridge_ui_label_set_text_if_changed(s_voice_sheet_hint, hint ? hint : "");
}

static void close_voice_sheet(void)
{
    if (s_voice_recording) {
        (void)fridge_audio_stop_recording();
    }
    s_voice_recording = false;
    s_voice_busy = false;
    s_voice_generation++;
    restore_wake_if_needed();
    if (s_voice_sheet) {
        lv_obj_delete(s_voice_sheet);
        s_voice_sheet = NULL;
        s_voice_sheet_status = NULL;
        s_voice_sheet_hint = NULL;
        s_voice_sheet_button_label = NULL;
    }
}

typedef struct {
    esp_err_t err;
    uint32_t generation;
    fridge_asr_result_t result;
} edit_food_asr_done_t;

static void edit_food_asr_done_async(void *user_data)
{
    edit_food_asr_done_t *done = (edit_food_asr_done_t *)user_data;
    if (!done) {
        return;
    }
    s_voice_busy = false;
    restore_wake_if_needed();
    if (!s_voice_sheet || done->generation != s_voice_generation) {
        free(done);
        return;
    }

    if (done->err == ESP_OK && done->result.text[0] != '\0') {
        edit_note_done(done->result.text);
        set_voice_sheet_state("继续录音", "识别完成，已写入补充信息", done->result.text);
    } else {
        const char *error = done->result.error[0] ? done->result.error : esp_err_to_name(done->err);
        set_voice_sheet_state("重新录音", error, "请靠近麦克风，录 1 到 6 秒");
    }
    free(done);
}

static void edit_food_asr_task(void *arg)
{
    edit_food_asr_done_t *done = calloc(1, sizeof(*done));
    if (!done) {
        ESP_LOGW(TAG, "ASR result allocation failed");
        s_voice_busy = false;
        vTaskDelete(NULL);
        return;
    }
    done->generation = (uint32_t)(uintptr_t)arg;
    done->err = fridge_asr_transcribe_latest_recording(&done->result);
    if (lv_async_call(edit_food_asr_done_async, done) != LV_RESULT_OK) {
        free(done);
    }
    vTaskDelete(NULL);
}

static void voice_sheet_record_cb(lv_event_t *event)
{
    (void)event;
    if (s_voice_busy || !s_voice_sheet) {
        return;
    }

    if (!s_voice_recording) {
        fridge_wake_word_status_t wake = {0};
        if (fridge_wake_word_get_status(&wake) == ESP_OK && wake.state == FRIDGE_WAKE_WORD_STATE_LISTENING) {
            s_wake_was_listening = true;
            esp_err_t wake_err = fridge_wake_word_stop();
            if (wake_err != ESP_OK) {
                set_voice_sheet_state("开始录音", "唤醒监听暂停失败", esp_err_to_name(wake_err));
                return;
            }
        }

        esp_err_t err = fridge_audio_start_recording();
        if (err != ESP_OK) {
            restore_wake_if_needed();
            set_voice_sheet_state("开始录音", err == ESP_ERR_INVALID_STATE ? "麦克风正被占用" : esp_err_to_name(err), "稍后再试");
            return;
        }
        s_voice_recording = true;
        set_voice_sheet_state("停止并识别", "正在录音", "可以说：数量改成 3 个，备注今天先吃");
        return;
    }

    esp_err_t err = fridge_audio_stop_recording();
    s_voice_recording = false;
    if (err != ESP_OK) {
        restore_wake_if_needed();
        set_voice_sheet_state("重新录音", esp_err_to_name(err), "录音停止失败");
        return;
    }

    s_voice_busy = true;
    s_voice_generation++;
    set_voice_sheet_state("识别中", "正在转文字...", "请稍等");
    BaseType_t ok = xTaskCreate(edit_food_asr_task, "ui_edit_asr", EDIT_FOOD_ASR_TASK_STACK,
                                (void *)(uintptr_t)s_voice_generation, 4, NULL);
    if (ok != pdPASS) {
        s_voice_busy = false;
        restore_wake_if_needed();
        set_voice_sheet_state("重新录音", "语音任务创建失败", "系统繁忙，请稍后再试");
    }
}

static void voice_sheet_cancel_cb(lv_event_t *event)
{
    (void)event;
    close_voice_sheet();
}

static void open_voice_sheet(void)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    close_voice_sheet();

    s_voice_sheet = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_voice_sheet, FRIDGE_DISPLAY_WIDTH, 356);
    lv_obj_align(s_voice_sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(s_voice_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_voice_sheet, theme->surface, 0);
    lv_obj_set_style_bg_opa(s_voice_sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_voice_sheet, theme->line, 0);
    lv_obj_set_style_border_width(s_voice_sheet, 1, 0);
    lv_obj_set_style_radius(s_voice_sheet, 28, 0);
    lv_obj_set_style_pad_all(s_voice_sheet, 0, 0);

    lv_obj_t *title = lv_label_create(s_voice_sheet);
    lv_label_set_text(title, "语音补充登记信息");
    lv_obj_set_style_text_color(title, theme->text, 0);
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 32, 24);

    lv_obj_t *cancel = lv_button_create(s_voice_sheet);
    lv_obj_set_size(cancel, 112, 48);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -30, 20);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xFFF0D0), 0);
    lv_obj_set_style_border_color(cancel, lv_color_hex(0x8A5D00), 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_set_style_radius(cancel, 24, 0);
    lv_obj_add_event_cb(cancel, voice_sheet_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "收起");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0x8A5D00), 0);
    lv_obj_set_style_text_font(cancel_label, fridge_ui_font_body(), 0);
    lv_obj_center(cancel_label);

    s_voice_sheet_status = lv_label_create(s_voice_sheet);
    lv_label_set_text(s_voice_sheet_status, "点下方按钮开始录音");
    lv_obj_set_style_text_color(s_voice_sheet_status, theme->muted, 0);
    lv_obj_set_style_text_font(s_voice_sheet_status, fridge_ui_font_body(), 0);
    lv_obj_align(s_voice_sheet_status, LV_ALIGN_TOP_LEFT, 34, 84);

    lv_obj_t *record = lv_button_create(s_voice_sheet);
    lv_obj_set_size(record, 300, 88);
    lv_obj_align(record, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_set_style_bg_color(record, theme->accent_2, 0);
    lv_obj_set_style_radius(record, 44, 0);
    lv_obj_set_style_shadow_width(record, 16, 0);
    lv_obj_set_style_shadow_color(record, lv_color_hex(0xD95745), 0);
    lv_obj_add_event_cb(record, voice_sheet_record_cb, LV_EVENT_CLICKED, NULL);
    s_voice_sheet_button_label = lv_label_create(record);
    lv_label_set_text(s_voice_sheet_button_label, "开始录音");
    lv_obj_set_style_text_color(s_voice_sheet_button_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_voice_sheet_button_label, fridge_ui_font_body(), 0);
    lv_obj_center(s_voice_sheet_button_label);

    s_voice_sheet_hint = lv_label_create(s_voice_sheet);
    lv_obj_set_size(s_voice_sheet_hint, 620, 70);
    lv_label_set_long_mode(s_voice_sheet_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_voice_sheet_hint, "可以说：数量改成 3 个，备注今天先吃");
    lv_obj_set_style_text_color(s_voice_sheet_hint, theme->accent, 0);
    lv_obj_set_style_text_font(s_voice_sheet_hint, fridge_ui_font_small(), 0);
    lv_obj_align(s_voice_sheet_hint, LV_ALIGN_TOP_MID, 0, 244);
}

static void voice_button_cb(lv_event_t *event)
{
    (void)event;
    open_voice_sheet();
}

static void field_cb(lv_event_t *event)
{
    uintptr_t field = (uintptr_t)lv_event_get_user_data(event);
    if (field == 0) {
        fridge_ui_keyboard_open_text("食材名称", s_editing.name, edit_name_done);
    } else if (field == 1) {
        fridge_ui_keyboard_open_text("数量", s_editing.quantity, edit_quantity_done);
    } else if (field == 2) {
        fridge_ui_keyboard_open_text("到期时间", s_editing.expire, edit_expire_done);
    } else if (field == 3) {
        fridge_ui_keyboard_open_text("放入时间", s_stored_text, edit_stored_done);
    } else if (field == 99) {
        fridge_ui_model_set_editing_draft(&s_editing);
        fridge_ui_model_begin_place_pick();
        fridge_ui_toast("请选择冰箱区域");
        fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
    } else {
        fridge_ui_keyboard_open_text("备注", s_note_text, edit_note_done);
    }
}

static void save_cb(lv_event_t *event)
{
    (void)event;
    if (s_editing.name[0] == '\0') {
        fridge_ui_toast("请先填写食材名称");
        return;
    }
    normalize_expire_days();
    set_location_value();
    fridge_ui_model_update_editing_food(&s_editing);
    close_voice_sheet();
    fridge_ui_toast("已保存");
    fridge_ui_show_page(FRIDGE_UI_PAGE_ZONE);
}

static void delete_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_model_delete_editing_food();
    close_voice_sheet();
    fridge_ui_toast("已清空该格");
    fridge_ui_show_page(FRIDGE_UI_PAGE_ZONE);
}

static void create_top_actions(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    s_top_actions = lv_obj_create(parent);
    lv_obj_remove_style_all(s_top_actions);
    lv_obj_set_pos(s_top_actions, 430, 6);
    lv_obj_set_size(s_top_actions, 248, 46);
    lv_obj_set_style_radius(s_top_actions, 999, 0);
    lv_obj_set_style_bg_color(s_top_actions, lv_color_hex(0xFFF8E7), 0);
    lv_obj_set_style_bg_opa(s_top_actions, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_top_actions, theme->line, 0);
    lv_obj_set_style_border_width(s_top_actions, 1, 0);
    lv_obj_set_style_pad_all(s_top_actions, 3, 0);

    lv_obj_t *del = lv_button_create(s_top_actions);
    lv_obj_set_size(del, 100, 36);
    lv_obj_set_pos(del, 5, 5);
    lv_obj_set_style_radius(del, 999, 0);
    lv_obj_set_style_shadow_width(del, 0, 0);
    lv_obj_set_style_bg_color(del, theme->danger, 0);
    lv_obj_add_event_cb(del, delete_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del_label = lv_label_create(del);
    lv_label_set_text(del_label, "删除");
    lv_obj_set_style_text_color(del_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(del_label, fridge_ui_font_small(), 0);
    lv_obj_center(del_label);

    lv_obj_t *save = lv_button_create(s_top_actions);
    lv_obj_set_size(save, 132, 36);
    lv_obj_set_pos(save, 110, 5);
    lv_obj_set_style_radius(save, 999, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_set_style_bg_color(save, theme->accent, 0);
    lv_obj_add_event_cb(save, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, "保存修改");
    lv_obj_set_style_text_color(save_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(save_label, fridge_ui_font_small(), 0);
    lv_obj_center(save_label);
}

static edit_field_view_t make_field(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, int16_t h,
                                    const char *title, uintptr_t field)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    edit_field_view_t view = {0};
    view.button = lv_button_create(parent);
    lv_obj_set_pos(view.button, x, y);
    lv_obj_set_size(view.button, w, h);
    lv_obj_set_style_bg_color(view.button, lv_color_hex(0xFFFEFA), 0);
    lv_obj_set_style_border_color(view.button, theme->line, 0);
    lv_obj_set_style_border_width(view.button, 1, 0);
    lv_obj_set_style_radius(view.button, 16, 0);
    lv_obj_set_style_shadow_width(view.button, 0, 0);
    lv_obj_set_style_pad_all(view.button, 0, 0);
    lv_obj_add_event_cb(view.button, field_cb, LV_EVENT_CLICKED, (void *)field);

    view.title = lv_label_create(view.button);
    lv_label_set_text(view.title, title);
    lv_obj_set_style_text_color(view.title, theme->accent, 0);
    lv_obj_set_style_text_font(view.title, fridge_ui_font_small(), 0);
    lv_obj_set_width(view.title, w >= 500 ? 122 : 84);
    lv_obj_set_style_text_align(view.title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(view.title, LV_ALIGN_LEFT_MID, 16, 0);

    view.value = lv_label_create(view.button);
    lv_label_set_text(view.value, "未填写");
    lv_label_set_long_mode(view.value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(view.value, w >= 500 ? w - 160 : w - 116);
    lv_obj_set_style_text_color(view.value, theme->text, 0);
    lv_obj_set_style_text_font(view.value, fridge_ui_font_body(), 0);
    lv_obj_set_style_text_align(view.value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(view.value, LV_ALIGN_RIGHT_MID, -16, 0);
    return view;
}

static void make_voice_editor(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, 42, 292);
    lv_obj_set_size(panel, 624, 258);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFEFA), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_70, 0);
    lv_obj_set_style_border_color(panel, theme->line, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 18, 0);

    lv_obj_t *voice = lv_button_create(panel);
    lv_obj_set_pos(voice, 10, 10);
    lv_obj_set_size(voice, 294, 48);
    lv_obj_set_style_bg_color(voice, theme->accent, 0);
    lv_obj_set_style_shadow_width(voice, 0, 0);
    lv_obj_set_style_radius(voice, 16, 0);
    lv_obj_add_event_cb(voice, voice_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *voice_label = lv_label_create(voice);
    lv_label_set_text(voice_label, "语音补充登记信息");
    lv_obj_set_style_text_color(voice_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(voice_label, fridge_ui_font_small(), 0);
    lv_obj_center(voice_label);

    lv_obj_t *manual = lv_button_create(panel);
    lv_obj_set_pos(manual, 320, 10);
    lv_obj_set_size(manual, 294, 48);
    lv_obj_set_style_bg_color(manual, lv_color_hex(0xFFF0D0), 0);
    lv_obj_set_style_border_color(manual, lv_color_hex(0x8A5D00), 0);
    lv_obj_set_style_border_width(manual, 1, 0);
    lv_obj_set_style_shadow_width(manual, 0, 0);
    lv_obj_set_style_radius(manual, 16, 0);
    lv_obj_add_event_cb(manual, field_cb, LV_EVENT_CLICKED, (void *)4);
    lv_obj_t *manual_label = lv_label_create(manual);
    lv_label_set_text(manual_label, "手动补充登记信息");
    lv_obj_set_style_text_color(manual_label, lv_color_hex(0x8A5D00), 0);
    lv_obj_set_style_text_font(manual_label, fridge_ui_font_small(), 0);
    lv_obj_center(manual_label);

    s_manual_preview = lv_label_create(panel);
    lv_obj_set_pos(s_manual_preview, 10, 68);
    lv_obj_set_size(s_manual_preview, 604, 136);
    lv_label_set_long_mode(s_manual_preview, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_color(s_manual_preview, lv_color_hex(0xFFF8E7), 0);
    lv_obj_set_style_bg_opa(s_manual_preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_manual_preview, theme->line, 0);
    lv_obj_set_style_border_width(s_manual_preview, 1, 0);
    lv_obj_set_style_radius(s_manual_preview, 14, 0);
    lv_obj_set_style_pad_left(s_manual_preview, 12, 0);
    lv_obj_set_style_pad_right(s_manual_preview, 12, 0);
    lv_obj_set_style_pad_top(s_manual_preview, 8, 0);
    lv_obj_set_style_text_color(s_manual_preview, theme->accent, 0);
    lv_obj_set_style_text_font(s_manual_preview, fridge_ui_font_small(), 0);

    s_voice_status = lv_label_create(panel);
    lv_obj_set_pos(s_voice_status, 16, 212);
    lv_obj_set_size(s_voice_status, 592, 30);
    lv_label_set_long_mode(s_voice_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_voice_status, theme->muted, 0);
    lv_obj_set_style_text_font(s_voice_status, fridge_ui_font_small(), 0);
    lv_label_set_text(s_voice_status, "可说：数量改成 3 个");
}

void fridge_ui_page_edit_food_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "食材信息编辑");
    lv_obj_set_style_text_color(kicker, theme->accent, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 4);

    s_title = lv_label_create(parent);
    lv_obj_set_style_text_color(s_title, theme->text, 0);
    lv_obj_set_style_text_font(s_title, fridge_ui_font_title(), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 36, 28);
    create_top_actions(parent);

    s_scroll = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_pos(s_scroll, 0, 66);
    lv_obj_set_size(s_scroll, FRIDGE_DISPLAY_WIDTH, 472);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);
    // 页面内容可能超过 72mm 小屏高度；只保留手指拖动滚动，关闭惯性/弹性以减少拖影。
    lv_obj_remove_flag(s_scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_bottom(s_scroll, 24, 0);

    s_name = make_field(s_scroll, 42, 0, 302, 66, "食材名称", 0);
    s_quantity = make_field(s_scroll, 364, 0, 302, 66, "数量", 1);
    s_expire = make_field(s_scroll, 42, 76, 302, 66, "到期时间", 2);
    s_location = make_field(s_scroll, 364, 76, 302, 66, "位置", 99);
    s_stored = make_field(s_scroll, 42, 152, 624, 60, "放入时间", 3);
    s_note = make_field(s_scroll, 42, 222, 624, 60, "备注", 4);
    make_voice_editor(s_scroll);

    lv_obj_t *del = lv_button_create(s_scroll);
    lv_obj_set_size(del, 180, 56);
    lv_obj_set_pos(del, 42, 568);
    lv_obj_set_style_bg_color(del, theme->danger, 0);
    lv_obj_set_style_radius(del, 16, 0);
    lv_obj_set_style_shadow_width(del, 0, 0);
    lv_obj_add_event_cb(del, delete_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del_label = lv_label_create(del);
    lv_label_set_text(del_label, "清空格子");
    lv_obj_set_style_text_color(del_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(del_label, fridge_ui_font_body(), 0);
    lv_obj_center(del_label);

    lv_obj_t *save = lv_button_create(s_scroll);
    lv_obj_set_size(save, 212, 56);
    lv_obj_set_pos(save, 454, 568);
    lv_obj_set_style_bg_color(save, theme->accent, 0);
    lv_obj_set_style_radius(save, 16, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, "保存修改");
    lv_obj_set_style_text_color(save_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(save_label, fridge_ui_font_body(), 0);
    lv_obj_center(save_label);
}

void fridge_ui_page_edit_food_update(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    s_editing = model.editing_food;
    if (s_last_zone != s_editing.zone || s_last_cell != s_editing.cell) {
        s_last_zone = s_editing.zone;
        s_last_cell = s_editing.cell;
        s_note_text[0] = '\0';
        s_stored_text[0] = '\0';
    }
    if (s_stored_text[0] == '\0') {
        make_default_stored_text(s_stored_text, sizeof(s_stored_text), s_editing.zone, s_editing.cell);
    }
    fridge_ui_label_set_text_if_changed(s_title, "修改记录");
    refresh_fields();
}
