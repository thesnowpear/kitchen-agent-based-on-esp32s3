// 冰箱小精灵输入键盘。
// 文本键盘提供常用词组和 ASR 语音输入；ASR 走后台任务，避免 HTTPS 转写阻塞 LVGL UI 线程。

#include "fridge_ui_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define KEYBOARD_ASR_TASK_STACK 8192
#define KEYBOARD_SCREEN_W 720
#define KEYBOARD_SCREEN_H 720
#define KEYBOARD_TOP_SAFE_PAD 40
#define KEYBOARD_IME_MAX_PINYIN 24
#define KEYBOARD_IME_MAX_CANDIDATES 12
#define KEYBOARD_HOT_WORD_COUNT 12
#define KEYBOARD_TEXT_KEY_H 44

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

typedef enum {
    KEY_ACTION_TEXT,
    KEY_ACTION_BACKSPACE,
    KEY_ACTION_SPACE,
    KEY_ACTION_CLEAR_PREEDIT,
    KEY_ACTION_MODE_TOGGLE,
} keyboard_key_action_t;

typedef struct {
    const char *label;
    const char *text;
    keyboard_key_action_t action;
} keyboard_key_t;

static const keyboard_key_t TEXT_KEYS_ROW_1[] = {
    {"q", "q", KEY_ACTION_TEXT},
    {"w", "w", KEY_ACTION_TEXT},
    {"e", "e", KEY_ACTION_TEXT},
    {"r", "r", KEY_ACTION_TEXT},
    {"t", "t", KEY_ACTION_TEXT},
    {"y", "y", KEY_ACTION_TEXT},
    {"u", "u", KEY_ACTION_TEXT},
    {"i", "i", KEY_ACTION_TEXT},
    {"o", "o", KEY_ACTION_TEXT},
    {"p", "p", KEY_ACTION_TEXT},
    {"退格", NULL, KEY_ACTION_BACKSPACE},
};

static const keyboard_key_t TEXT_KEYS_ROW_NUM[] = {
    {"1", "1", KEY_ACTION_TEXT},
    {"2", "2", KEY_ACTION_TEXT},
    {"3", "3", KEY_ACTION_TEXT},
    {"4", "4", KEY_ACTION_TEXT},
    {"5", "5", KEY_ACTION_TEXT},
    {"6", "6", KEY_ACTION_TEXT},
    {"7", "7", KEY_ACTION_TEXT},
    {"8", "8", KEY_ACTION_TEXT},
    {"9", "9", KEY_ACTION_TEXT},
    {"0", "0", KEY_ACTION_TEXT},
};

static const keyboard_key_t TEXT_KEYS_ROW_2[] = {
    {"a", "a", KEY_ACTION_TEXT},
    {"s", "s", KEY_ACTION_TEXT},
    {"d", "d", KEY_ACTION_TEXT},
    {"f", "f", KEY_ACTION_TEXT},
    {"g", "g", KEY_ACTION_TEXT},
    {"h", "h", KEY_ACTION_TEXT},
    {"j", "j", KEY_ACTION_TEXT},
    {"k", "k", KEY_ACTION_TEXT},
    {"l", "l", KEY_ACTION_TEXT},
};

static const keyboard_key_t TEXT_KEYS_ROW_3[] = {
    {"z", "z", KEY_ACTION_TEXT},
    {"x", "x", KEY_ACTION_TEXT},
    {"c", "c", KEY_ACTION_TEXT},
    {"v", "v", KEY_ACTION_TEXT},
    {"b", "b", KEY_ACTION_TEXT},
    {"n", "n", KEY_ACTION_TEXT},
    {"m", "m", KEY_ACTION_TEXT},
    {".", ".", KEY_ACTION_TEXT},
    {",", ",", KEY_ACTION_TEXT},
    {":", ":", KEY_ACTION_TEXT},
};

static const keyboard_key_t TEXT_KEYS_ROW_4[] = {
    {"中英", NULL, KEY_ACTION_MODE_TOGGLE},
    {"清拼", NULL, KEY_ACTION_CLEAR_PREEDIT},
    {"空格", NULL, KEY_ACTION_SPACE},
    {"-", "-", KEY_ACTION_TEXT},
    {"退格", NULL, KEY_ACTION_BACKSPACE},
};

typedef struct {
    const char *pinyin;
    const char *chars;
} ime_entry_t;

typedef struct {
    const char *label;
    const char *text;
} ime_hot_word_t;

static const ime_hot_word_t IME_HOT_WORDS[KEYBOARD_HOT_WORD_COUNT] = {
    {"番茄", "番茄"},
    {"鸡蛋", "鸡蛋"},
    {"牛奶", "牛奶"},
    {"菠菜", "菠菜"},
    {"黄瓜", "黄瓜"},
    {"酸奶", "酸奶"},
    {"生菜", "生菜"},
    {"蘑菇", "蘑菇"},
    {"豆腐", "豆腐"},
    {"苹果", "苹果"},
    {"今天", "今天"},
    {"明天", "明天"},
};

static const ime_entry_t IME_ENTRIES[] = {
    {"fan", "番饭范反烦返"},
    {"cai", "菜材财采彩裁才"},
    {"ji", "鸡几及记机急级集吉季剂迹"},
    {"dan", "蛋但单担丹淡胆"},
    {"niu", "牛纽扭"},
    {"nai", "奶耐乃"},
    {"huang", "黄"},
    {"gua", "瓜"},
    {"suan", "酸算蒜"},
    {"sheng", "生声省升胜剩盛"},
    {"mogu", "蘑菇"},
    {"dou", "豆都斗兜"},
    {"fu", "腐付复福富夫辅"},
    {"ping", "苹瓶平品评"},
    {"guo", "果国过锅"},
    {"zuo", "作做左坐昨佐"},
    {"jin", "今进近金紧尽"},
    {"ming", "明名命"},
    {"tian", "天田甜添填"},
    {"you", "有又右优油友由"},
    {"lai", "来赖"},
    {"qu", "去取区趣"},
    {"duo", "多夺躲朵"},
    {"shao", "少烧勺稍"},
    {"liang", "量两良亮凉粮"},
    {"ge", "个各格哥歌隔"},
    {"ke", "可克颗刻客科"},
    {"ben", "本奔笨"},
    {"ban", "半板班般办"},
    {"wan", "完晚碗万"},
    {"dao", "倒到道导"},
    {"jia", "加家佳夹"},
    {"jian", "减见件间剪建简"},
    {"kai", "开"},
    {"guan", "关观管"},
    {"fang", "放方房防"},
    {"na", "拿哪纳"},
    {"xiao", "小消笑效"},
    {"cha", "茶查炒插"},
    {"chao", "炒超朝潮"},
    {"zhu", "煮主住筑"},
    {"zheng", "蒸正争整证"},
    {"men", "门们闷"},
    {"dun", "炖顿盾"},
    {"shui", "水谁睡税"},
    {"zhe", "这者着折"},
    {"zhi", "只之直知治至志质值纸"},
    {"shi", "食时是事十实室市石式使世示始识试适视势"},
    {"li", "里李理力立离礼利丽粒"},
    {"hao", "好号耗"},
    {"xie", "写洗谢协械"},
};

static char s_ime_pinyin[KEYBOARD_IME_MAX_PINYIN + 1];
static bool s_ime_ascii_mode;
static lv_obj_t *s_ime_candidate_panel;
static char s_candidate_texts[KEYBOARD_IME_MAX_CANDIDATES][8];
static size_t s_candidate_count;

static void ime_refresh_hint(void);
static void ime_render_candidate_panel(void);
static void ime_commit_candidate_text(const char *text);
static void ime_backspace(void);
static void ime_input_text(const char *text);
static void ime_toggle_mode(void);
static void ime_reset_preedit(void);
static bool ime_try_commit_suggestion(void);

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
    s_ime_candidate_panel = NULL;
    ime_reset_preedit();
    s_ime_ascii_mode = false;
    s_candidate_count = 0;
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
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xFFF0D0), 0);
    lv_obj_set_style_border_color(cancel, lv_color_hex(0x8A5D00), 0);
    lv_obj_set_style_border_width(cancel, 2, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0x8A5D00), 0);
    lv_obj_set_style_text_font(cancel_label, fridge_ui_font_body(), 0);
    lv_obj_center(cancel_label);
}

static void candidate_cb(lv_event_t *event)
{
    const char *word = lv_event_get_user_data(event);
    if (word && s_textarea) {
        ime_commit_candidate_text(word);
    }
}

static void text_key_cb(lv_event_t *event)
{
    const keyboard_key_t *key = lv_event_get_user_data(event);
    if (!key || !s_textarea) {
        return;
    }

    switch (key->action) {
    case KEY_ACTION_BACKSPACE:
        ime_backspace();
        break;
    case KEY_ACTION_SPACE:
        if (s_ime_ascii_mode || s_ime_pinyin[0] == '\0') {
            lv_textarea_add_text(s_textarea, " ");
        } else {
            if (!ime_try_commit_suggestion()) {
                lv_textarea_add_text(s_textarea, " ");
            }
        }
        ime_refresh_hint();
        break;
    case KEY_ACTION_CLEAR_PREEDIT:
        ime_reset_preedit();
        ime_refresh_hint();
        ime_render_candidate_panel();
        break;
    case KEY_ACTION_MODE_TOGGLE:
        ime_toggle_mode();
        break;
    case KEY_ACTION_TEXT:
    default:
        ime_input_text(key->text);
        break;
    }
}

static lv_obj_t *create_text_key(lv_obj_t *parent, const keyboard_key_t *key, int16_t x, int16_t y, int16_t w)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, KEYBOARD_TEXT_KEY_H);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xF0F6EC), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xB8C7B0), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, text_key_cb, LV_EVENT_CLICKED, (void *)key);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, key->label);
    lv_obj_set_style_text_color(label, theme->text, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_for_text(fridge_ui_font_small(), key->label), 0);
    lv_obj_center(label);
    return btn;
}

static void create_text_key_row(lv_obj_t *parent, const keyboard_key_t *keys, size_t count,
                                int16_t x, int16_t y, int16_t key_w, int16_t gap)
{
    for (size_t i = 0; i < count; i++) {
        create_text_key(parent, &keys[i], x + (int16_t)i * (key_w + gap), y, key_w);
    }
}

// 单字拼音输入法只保留厨房常用高频字和少量功能字，字典小而够用。
// 这里不做完整词库联想，避免把固件空间和 UI 复杂度一起抬高。
static bool ime_utf8_copy_char(const char **cursor, char *out, size_t out_size)
{
    if (!cursor || !*cursor || !out || out_size < 2) {
        return false;
    }
    const unsigned char *p = (const unsigned char *)*cursor;
    if (*p == '\0') {
        return false;
    }

    size_t len = 1;
    if (*p < 0x80) {
        len = 1;
    } else if ((*p & 0xE0) == 0xC0) {
        len = 2;
    } else if ((*p & 0xF0) == 0xE0) {
        len = 3;
    } else if ((*p & 0xF8) == 0xF0) {
        len = 4;
    } else {
        return false;
    }

    if (len + 1 > out_size) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = (char)p[i];
    }
    out[len] = '\0';
    *cursor += len;
    return true;
}

static void ime_reset_preedit(void)
{
    s_ime_pinyin[0] = '\0';
}

static void ime_refresh_hint(void)
{
    char hint[96] = {0};
    if (s_ime_ascii_mode) {
        if (s_ime_pinyin[0] != '\0') {
            snprintf(hint, sizeof(hint), "英文模式 | 拼音缓冲：%s", s_ime_pinyin);
        } else {
            strlcpy(hint, "英文模式 | 字母直接输入", sizeof(hint));
        }
    } else if (s_ime_pinyin[0] != '\0') {
        snprintf(hint, sizeof(hint), "拼音模式 | 当前：%s", s_ime_pinyin);
    } else {
        strlcpy(hint, "拼音模式 | 常用食材 / 输入字母选字", sizeof(hint));
    }
    set_status_text(hint);
}

static void ime_set_candidate_panel(lv_obj_t *panel)
{
    s_ime_candidate_panel = panel;
}

static void ime_clear_candidate_panel(void)
{
    if (s_ime_candidate_panel) {
        lv_obj_clean(s_ime_candidate_panel);
    }
    s_candidate_count = 0;
}

static void ime_append_candidate(const char *text)
{
    if (!text || text[0] == '\0' || s_candidate_count >= KEYBOARD_IME_MAX_CANDIDATES) {
        return;
    }
    strlcpy(s_candidate_texts[s_candidate_count], text, sizeof(s_candidate_texts[s_candidate_count]));
    s_candidate_count++;
}

static void ime_collect_candidates_from_entries(void)
{
    size_t pinyin_len = strlen(s_ime_pinyin);
    for (size_t i = 0; i < sizeof(IME_ENTRIES) / sizeof(IME_ENTRIES[0]); i++) {
        const char *entry = IME_ENTRIES[i].pinyin;
        if (s_ime_pinyin[0] == '\0') {
            continue;
        }
        if (strncmp(entry, s_ime_pinyin, pinyin_len) != 0) {
            continue;
        }
        const char *cursor = IME_ENTRIES[i].chars;
        while (*cursor && s_candidate_count < KEYBOARD_IME_MAX_CANDIDATES) {
            char ch[8] = {0};
            if (!ime_utf8_copy_char(&cursor, ch, sizeof(ch))) {
                break;
            }
            ime_append_candidate(ch);
        }
        if (s_candidate_count >= KEYBOARD_IME_MAX_CANDIDATES) {
            return;
        }
    }
}

static void ime_render_candidate_panel(void)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    ime_clear_candidate_panel();
    if (!s_ime_candidate_panel) {
        return;
    }

    lv_obj_set_style_pad_column(s_ime_candidate_panel, 6, 0);
    lv_obj_set_style_pad_row(s_ime_candidate_panel, 6, 0);

    if (s_ime_ascii_mode) {
        // 英文模式时候选区不做联想，只提示当前键盘状态，避免误触选字。
        lv_obj_t *tip = lv_label_create(s_ime_candidate_panel);
        lv_label_set_text(tip, "英文模式：字母直接输入");
        lv_obj_set_style_text_color(tip, theme->muted, 0);
        lv_obj_set_style_text_font(tip, fridge_ui_font_ai_body(), 0);
        lv_obj_center(tip);
        return;
    }

    if (s_ime_pinyin[0] == '\0') {
        for (size_t i = 0; i < KEYBOARD_HOT_WORD_COUNT; i++) {
            lv_obj_t *btn = lv_button_create(s_ime_candidate_panel);
            lv_obj_set_size(btn, 108, 42);
            lv_obj_set_style_radius(btn, 12, 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0xEEF4E7), 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(0x5F8F72), 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_add_event_cb(btn, candidate_cb, LV_EVENT_CLICKED, (void *)IME_HOT_WORDS[i].text);
            lv_obj_t *word = lv_label_create(btn);
            lv_label_set_text(word, IME_HOT_WORDS[i].label);
            lv_obj_set_style_text_color(word, theme->text, 0);
            lv_obj_set_style_text_font(word, fridge_ui_font_ai_body(), 0);
            lv_obj_center(word);
        }
        return;
    } else {
        ime_collect_candidates_from_entries();
    }

    if (s_candidate_count == 0) {
        lv_obj_t *tip = lv_label_create(s_ime_candidate_panel);
        lv_label_set_text(tip, "没有可用候选");
        lv_obj_set_style_text_color(tip, theme->muted, 0);
        lv_obj_set_style_text_font(tip, fridge_ui_font_ai_body(), 0);
        lv_obj_center(tip);
        return;
    }

    for (size_t i = 0; i < s_candidate_count; i++) {
        lv_obj_t *btn = lv_button_create(s_ime_candidate_panel);
        lv_obj_set_size(btn, 108, 42);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xEEF4E7), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x5F8F72), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, candidate_cb, LV_EVENT_CLICKED, (void *)s_candidate_texts[i]);
        lv_obj_t *word = lv_label_create(btn);
        lv_label_set_text(word, s_candidate_texts[i]);
        lv_obj_set_style_text_color(word, theme->text, 0);
        lv_obj_set_style_text_font(word, fridge_ui_font_ai_body(), 0);
        lv_obj_center(word);
    }
}

static void ime_commit_candidate_text(const char *text)
{
    if (!text || text[0] == '\0' || !s_textarea) {
        return;
    }
    lv_textarea_add_text(s_textarea, text);
    ime_reset_preedit();
    ime_refresh_hint();
    ime_render_candidate_panel();
}

static bool ime_try_commit_suggestion(void)
{
    if (s_candidate_count == 0) {
        ime_render_candidate_panel();
    }
    if (s_candidate_count == 0) {
        return false;
    }
    ime_commit_candidate_text(s_candidate_texts[0]);
    return true;
}

static void ime_backspace(void)
{
    if (s_ime_ascii_mode) {
        if (s_textarea) {
            lv_textarea_delete_char(s_textarea);
        }
        return;
    }

    if (s_ime_pinyin[0] != '\0') {
        size_t len = strlen(s_ime_pinyin);
        if (len > 0) {
            s_ime_pinyin[len - 1] = '\0';
            ime_refresh_hint();
            ime_render_candidate_panel();
        }
        return;
    }

    if (s_textarea) {
        lv_textarea_delete_char(s_textarea);
    }
}

static void ime_input_text(const char *text)
{
    if (!text || text[0] == '\0') {
        return;
    }

    if (s_ime_ascii_mode) {
        lv_textarea_add_text(s_textarea, text);
        return;
    }

    if (strlen(text) == 1 && isalpha((unsigned char)text[0])) {
        size_t len = strlen(s_ime_pinyin);
        if (len < KEYBOARD_IME_MAX_PINYIN) {
            s_ime_pinyin[len] = (char)tolower((unsigned char)text[0]);
            s_ime_pinyin[len + 1] = '\0';
            ime_refresh_hint();
            ime_render_candidate_panel();
        }
        return;
    }

    lv_textarea_add_text(s_textarea, text);
    ime_reset_preedit();
    ime_refresh_hint();
    ime_render_candidate_panel();
}

static void ime_toggle_mode(void)
{
    s_ime_ascii_mode = !s_ime_ascii_mode;
    ime_reset_preedit();
    ime_refresh_hint();
    ime_render_candidate_panel();
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
        ime_reset_preedit();
        ime_render_candidate_panel();
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
    s_ime_ascii_mode = false;
    ime_reset_preedit();
    ime_clear_candidate_panel();

    s_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_modal, KEYBOARD_SCREEN_W, KEYBOARD_SCREEN_H);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_modal, theme->bg, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);

    lv_obj_t *label = lv_label_create(s_modal);
    lv_label_set_text(label, title ? title : "手动输入");
    lv_obj_set_style_text_color(label, theme->text, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_for_text(fridge_ui_font_body(), title ? title : "手动输入"), 0);
    lv_obj_set_pos(label, 18, KEYBOARD_TOP_SAFE_PAD + 8);

    s_textarea = lv_textarea_create(s_modal);
    lv_obj_set_size(s_textarea, 684, 56);
    lv_obj_set_pos(s_textarea, 18, KEYBOARD_TOP_SAFE_PAD + 46);
    lv_textarea_set_one_line(s_textarea, true);
    lv_textarea_set_text(s_textarea, initial_text ? initial_text : "");
    lv_obj_set_style_radius(s_textarea, 12, 0);
    lv_obj_set_style_border_color(s_textarea, theme->line, 0);
    lv_obj_set_style_border_width(s_textarea, 1, 0);
    lv_obj_set_style_text_font(s_textarea, fridge_ui_font_body(), 0);
    lv_obj_set_style_pad_top(s_textarea, 6, 0);
    lv_obj_set_style_pad_bottom(s_textarea, 6, 0);

    s_status_label = lv_label_create(s_modal);
    lv_label_set_text(s_status_label, "手动输入 / 语音录入");
    lv_obj_set_style_text_color(s_status_label, theme->muted, 0);
    lv_obj_set_style_text_font(s_status_label, fridge_ui_font_ai_body(), 0);
    lv_obj_set_size(s_status_label, 684, 24);
    lv_obj_set_pos(s_status_label, 22, KEYBOARD_TOP_SAFE_PAD + 108);

    s_voice_btn = lv_button_create(s_modal);
    lv_obj_set_size(s_voice_btn, 78, 78);
    lv_obj_set_pos(s_voice_btn, 321, 618);
    lv_obj_set_style_bg_color(s_voice_btn, theme->accent_2, 0);
    lv_obj_set_style_shadow_width(s_voice_btn, 0, 0);
    lv_obj_set_style_radius(s_voice_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(s_voice_btn, voice_cb, LV_EVENT_CLICKED, NULL);
    s_voice_label = lv_label_create(s_voice_btn);
    lv_label_set_text(s_voice_label, "语音");
    lv_obj_set_style_text_color(s_voice_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_voice_label, fridge_ui_font_ai_body(), 0);
    lv_obj_center(s_voice_label);

    lv_obj_t *candidates = lv_obj_create(s_modal);
    lv_obj_remove_style_all(candidates);
    lv_obj_set_size(candidates, 684, 132);
    lv_obj_set_pos(candidates, 18, KEYBOARD_TOP_SAFE_PAD + 150);
    lv_obj_clear_flag(candidates, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(candidates, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(candidates, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(candidates, 6, 0);
    lv_obj_set_style_pad_row(candidates, 6, 0);
    ime_set_candidate_panel(candidates);
    ime_refresh_hint();
    ime_render_candidate_panel();

    lv_obj_t *keyboard = lv_obj_create(s_modal);
    // 内置 lv_keyboard 在当前实屏上只绘制白底，这里改用普通按钮自建键盘，保证按键可见可点。
    lv_obj_remove_style_all(keyboard);
    lv_obj_set_size(keyboard, 684, 276);
    lv_obj_set_pos(keyboard, 18, 326);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(keyboard, theme->bg, 0);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, 0);
    // 五行按键压在原键盘区域内，避免新增数字行后挤到语音和确认按钮。
    create_text_key_row(keyboard, TEXT_KEYS_ROW_NUM, sizeof(TEXT_KEYS_ROW_NUM) / sizeof(TEXT_KEYS_ROW_NUM[0]), 31, 0, 56, 6);
    create_text_key_row(keyboard, TEXT_KEYS_ROW_1, sizeof(TEXT_KEYS_ROW_1) / sizeof(TEXT_KEYS_ROW_1[0]), 0, 52, 56, 6);
    create_text_key_row(keyboard, TEXT_KEYS_ROW_2, sizeof(TEXT_KEYS_ROW_2) / sizeof(TEXT_KEYS_ROW_2[0]), 31, 104, 56, 6);
    create_text_key_row(keyboard, TEXT_KEYS_ROW_3, sizeof(TEXT_KEYS_ROW_3) / sizeof(TEXT_KEYS_ROW_3[0]), 0, 156, 56, 6);
    create_text_key_row(keyboard, TEXT_KEYS_ROW_4, sizeof(TEXT_KEYS_ROW_4) / sizeof(TEXT_KEYS_ROW_4[0]), 84, 208, 90, 8);

    lv_obj_t *done = lv_button_create(s_modal);
    lv_obj_set_size(done, 178, 66);
    lv_obj_set_pos(done, 524, 626);
    lv_obj_set_style_bg_color(done, theme->accent, 0);
    lv_obj_set_style_shadow_width(done, 0, 0);
    lv_obj_set_style_radius(done, 16, 0);
    lv_obj_add_event_cb(done, text_done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_label = lv_label_create(done);
    lv_label_set_text(done_label, "确定");
    lv_obj_set_style_text_font(done_label, fridge_ui_font_ai_body(), 0);
    lv_obj_center(done_label);

    lv_obj_t *cancel = lv_button_create(s_modal);
    lv_obj_set_size(cancel, 178, 66);
    lv_obj_set_pos(cancel, 18, 626);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xFFF0D0), 0);
    lv_obj_set_style_border_color(cancel, lv_color_hex(0x8A5D00), 0);
    lv_obj_set_style_border_width(cancel, 2, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_set_style_radius(cancel, 16, 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0x8A5D00), 0);
    lv_obj_set_style_text_font(cancel_label, fridge_ui_font_ai_body(), 0);
    lv_obj_center(cancel_label);
}
