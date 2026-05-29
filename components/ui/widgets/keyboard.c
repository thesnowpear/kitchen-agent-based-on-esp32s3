// 冰箱小精灵输入键盘。
// 文本键盘提供常用词组和 ASR 语音输入；ASR 走后台任务，避免 HTTPS 转写阻塞 LVGL UI 线程。

#include "fridge_ui_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define KEYBOARD_ASR_TASK_STACK 8192

static const char *TAG = "fridge_ui_keyboard";

static lv_obj_t *s_modal;
static lv_obj_t *s_textarea;
static lv_obj_t *s_voice_label;
static lv_obj_t *s_voice_btn;
static lv_obj_t *s_status_label;
static char s_ssid[33];
static void (*s_text_done_cb)(const char *text);
static bool s_voice_recording;
static bool s_voice_busy;
static uint32_t s_voice_generation;

typedef struct {
    const char *key;
    const char *value;
} pinyin_word_t;

static const pinyin_word_t PINYIN_WORDS[] = {
    {"fanqie", "番茄"},
    {"jidan", "鸡蛋"},
    {"niunai", "牛奶"},
    {"bocai", "菠菜"},
    {"huanggua", "黄瓜"},
    {"suannai", "酸奶"},
    {"shengcai", "生菜"},
    {"mogu", "蘑菇"},
    {"doufu", "豆腐"},
    {"pingguo", "苹果"},
    {"zuotian", "昨天"},
    {"jintian", "今天"},
    {"mingtian", "明天"},
    {"tianhou", "天后"},
    {"youxian", "优先使用"},
    {"baoxian", "保鲜"},
};

static void close_modal(void)
{
    if (s_voice_recording) {
        (void)fridge_audio_stop_recording();
    }
    s_voice_recording = false;
    s_voice_busy = false;
    s_voice_generation++;
    if (s_modal) {
        lv_obj_delete(s_modal);
        s_modal = NULL;
        s_textarea = NULL;
        s_voice_label = NULL;
        s_voice_btn = NULL;
        s_status_label = NULL;
    }
}

static void set_status_text(const char *text)
{
    if (s_status_label) {
        fridge_ui_label_set_text_if_changed(s_status_label, text ? text : "");
    }
}

static void set_voice_button_text(const char *text)
{
    if (s_voice_label) {
        fridge_ui_label_set_text_if_changed(s_voice_label, text ? text : "语音");
    }
}

static void connect_cb(lv_event_t *event)
{
    (void)event;
    const char *password = s_textarea ? lv_textarea_get_text(s_textarea) : "";
    fridge_ui_model_connect_wifi_async(s_ssid, password);
    close_modal();
}

static void text_done_cb(lv_event_t *event)
{
    (void)event;
    const char *text = s_textarea ? lv_textarea_get_text(s_textarea) : "";
    if (s_text_done_cb) {
        s_text_done_cb(text);
    }
    close_modal();
}

static void cancel_cb(lv_event_t *event)
{
    (void)event;
    close_modal();
}

void fridge_ui_keyboard_open(const char *title, const char *ssid)
{
    (void)title;
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    strlcpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid));
    close_modal();

    s_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_modal, 640, 520);
    lv_obj_center(s_modal);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_modal, theme->surface, 0);
    lv_obj_set_style_radius(s_modal, 8, 0);
    lv_obj_set_style_border_color(s_modal, theme->line, 0);
    lv_obj_set_style_border_width(s_modal, 1, 0);

    lv_obj_t *label = lv_label_create(s_modal);
    lv_label_set_text_fmt(label, "连接 %s", s_ssid);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 8);

    s_textarea = lv_textarea_create(s_modal);
    lv_obj_set_size(s_textarea, 590, 64);
    lv_obj_align(s_textarea, LV_ALIGN_TOP_MID, 0, 54);
    lv_textarea_set_password_mode(s_textarea, true);
    lv_textarea_set_one_line(s_textarea, true);
    lv_textarea_set_placeholder_text(s_textarea, "输入密码");

    lv_obj_t *keyboard = lv_keyboard_create(s_modal);
    lv_obj_set_size(keyboard, 590, 285);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -72);
    lv_keyboard_set_textarea(keyboard, s_textarea);

    lv_obj_t *connect = lv_button_create(s_modal);
    lv_obj_set_size(connect, 160, 52);
    lv_obj_align(connect, LV_ALIGN_BOTTOM_RIGHT, -24, -10);
    lv_obj_set_style_bg_color(connect, theme->accent, 0);
    lv_obj_add_event_cb(connect, connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = lv_label_create(connect);
    lv_label_set_text(connect_label, "连接");
    lv_obj_center(connect_label);

    lv_obj_t *cancel = lv_button_create(s_modal);
    lv_obj_set_size(cancel, 120, 52);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 24, -10);
    lv_obj_set_style_bg_color(cancel, theme->surface_soft, 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_center(cancel_label);
}

static void candidate_cb(lv_event_t *event)
{
    const char *word = lv_event_get_user_data(event);
    if (word && s_textarea) {
        lv_textarea_add_text(s_textarea, word);
    }
}

typedef struct {
    esp_err_t err;
    uint32_t generation;
    fridge_asr_result_t result;
} keyboard_asr_done_t;

static void keyboard_asr_done_async(void *user_data)
{
    keyboard_asr_done_t *done = (keyboard_asr_done_t *)user_data;
    if (!done) {
        return;
    }
    s_voice_busy = false;
    set_voice_button_text("语音");
    if (!s_modal || !s_textarea || done->generation != s_voice_generation) {
        free(done);
        return;
    }
    if (done->err == ESP_OK && done->result.text[0] != '\0') {
        lv_textarea_set_text(s_textarea, done->result.text);
        set_status_text("语音已填入文本");
    } else {
        set_status_text(done->result.error[0] ? done->result.error : esp_err_to_name(done->err));
    }
    free(done);
}

static void keyboard_asr_task(void *arg)
{
    (void)arg;
    keyboard_asr_done_t *done = calloc(1, sizeof(*done));
    if (!done) {
        ESP_LOGW(TAG, "ASR result allocation failed");
        s_voice_busy = false;
        vTaskDelete(NULL);
    }
    done->generation = s_voice_generation;
    done->err = fridge_asr_transcribe_latest_recording(&done->result);
    if (lv_async_call(keyboard_asr_done_async, done) != LV_RESULT_OK) {
        free(done);
    }
    vTaskDelete(NULL);
}

static void voice_cb(lv_event_t *event)
{
    (void)event;
    if (!s_modal || s_voice_busy) {
        return;
    }
    if (!s_voice_recording) {
        esp_err_t err = fridge_audio_start_recording();
        if (err != ESP_OK) {
            set_status_text(err == ESP_ERR_INVALID_STATE ? "麦克风正被占用" : esp_err_to_name(err));
            return;
        }
        s_voice_recording = true;
        set_voice_button_text("停止");
        set_status_text("正在录音，再点一次转文字");
        return;
    }

    esp_err_t err = fridge_audio_stop_recording();
    s_voice_recording = false;
    if (err != ESP_OK) {
        set_voice_button_text("语音");
        set_status_text(esp_err_to_name(err));
        return;
    }
    s_voice_busy = true;
    s_voice_generation++;
    set_voice_button_text("转写中");
    set_status_text("正在识别语音...");
    BaseType_t ok = xTaskCreate(keyboard_asr_task, "ui_asr", KEYBOARD_ASR_TASK_STACK, NULL, 4, NULL);
    if (ok != pdPASS) {
        s_voice_busy = false;
        set_voice_button_text("语音");
        set_status_text("语音任务创建失败");
    }
}

void fridge_ui_keyboard_open_text(const char *title, const char *initial_text, void (*done_cb)(const char *text))
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    close_modal();
    s_text_done_cb = done_cb;

    s_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_modal, 690, 650);
    lv_obj_center(s_modal);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_modal, theme->surface, 0);
    lv_obj_set_style_radius(s_modal, 8, 0);
    lv_obj_set_style_border_color(s_modal, theme->line, 0);
    lv_obj_set_style_border_width(s_modal, 1, 0);

    lv_obj_t *label = lv_label_create(s_modal);
    lv_label_set_text(label, title ? title : "手动输入");
    lv_obj_set_style_text_color(label, theme->text, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_body(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 18, 12);

    s_textarea = lv_textarea_create(s_modal);
    lv_obj_set_size(s_textarea, 648, 74);
    lv_obj_align(s_textarea, LV_ALIGN_TOP_MID, 0, 54);
    lv_textarea_set_one_line(s_textarea, true);
    lv_textarea_set_text(s_textarea, initial_text ? initial_text : "");

    s_status_label = lv_label_create(s_modal);
    lv_label_set_text(s_status_label, "可手动输入，也可点语音录入");
    lv_obj_set_style_text_color(s_status_label, theme->muted, 0);
    lv_obj_set_style_text_font(s_status_label, fridge_ui_font_small(), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 22, 134);

    s_voice_btn = lv_button_create(s_modal);
    lv_obj_set_size(s_voice_btn, 144, 48);
    lv_obj_align(s_voice_btn, LV_ALIGN_TOP_RIGHT, -22, 124);
    lv_obj_set_style_bg_color(s_voice_btn, theme->accent_2, 0);
    lv_obj_set_style_shadow_width(s_voice_btn, 0, 0);
    lv_obj_set_style_radius(s_voice_btn, 14, 0);
    lv_obj_add_event_cb(s_voice_btn, voice_cb, LV_EVENT_CLICKED, NULL);
    s_voice_label = lv_label_create(s_voice_btn);
    lv_label_set_text(s_voice_label, "语音");
    lv_obj_set_style_text_color(s_voice_label, lv_color_white(), 0);
    lv_obj_center(s_voice_label);

    lv_obj_t *candidates = lv_obj_create(s_modal);
    lv_obj_remove_style_all(candidates);
    lv_obj_set_size(candidates, 648, 122);
    lv_obj_align(candidates, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_set_flex_flow(candidates, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(candidates, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (size_t i = 0; i < sizeof(PINYIN_WORDS) / sizeof(PINYIN_WORDS[0]); i++) {
        lv_obj_t *btn = lv_button_create(candidates);
        lv_obj_set_size(btn, 104, 40);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xEEF4E7), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x5F8F72), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, candidate_cb, LV_EVENT_CLICKED, (void *)PINYIN_WORDS[i].value);
        lv_obj_t *word = lv_label_create(btn);
        lv_label_set_text(word, PINYIN_WORDS[i].value);
        lv_obj_set_style_text_color(word, theme->text, 0);
        lv_obj_set_style_text_font(word, fridge_ui_font_small(), 0);
        lv_obj_center(word);
    }

    lv_obj_t *keyboard = lv_keyboard_create(s_modal);
    lv_obj_set_size(keyboard, 648, 270);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -66);
    lv_keyboard_set_textarea(keyboard, s_textarea);

    lv_obj_t *done = lv_button_create(s_modal);
    lv_obj_set_size(done, 174, 52);
    lv_obj_align(done, LV_ALIGN_BOTTOM_RIGHT, -24, -10);
    lv_obj_set_style_bg_color(done, theme->accent, 0);
    lv_obj_add_event_cb(done, text_done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_label = lv_label_create(done);
    lv_label_set_text(done_label, "确定");
    lv_obj_center(done_label);

    lv_obj_t *cancel = lv_button_create(s_modal);
    lv_obj_set_size(cancel, 132, 52);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 24, -10);
    lv_obj_set_style_bg_color(cancel, theme->surface_soft, 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_center(cancel_label);
}
