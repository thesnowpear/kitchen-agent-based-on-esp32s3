// 冰箱小精灵待机页。
// 显示大时钟、环境亮度和雷达人 presence；自动息屏第一阶段只降亮度，不进入深睡，避免影响 USB/Web 调试。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <time.h>

#include "esp_timer.h"
#include "fridge_display.h"

static lv_obj_t *s_clock;
static lv_obj_t *s_face;
static lv_obj_t *s_hint;
static uint8_t s_face_index;
static int64_t s_last_face_ms;

static const char *FACES[] = {":)", "^_^", "-_-", "o_o"};

static void wake_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
}

void fridge_ui_page_standby_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_add_event_cb(parent, wake_cb, LV_EVENT_CLICKED, NULL);

    s_clock = lv_label_create(parent);
    lv_obj_set_style_text_color(s_clock, theme->text, 0);
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, 62);

    s_face = lv_label_create(parent);
    lv_obj_set_style_text_color(s_face, theme->accent, 0);
    lv_obj_align(s_face, LV_ALIGN_CENTER, 0, -8);

    s_hint = lv_label_create(parent);
    lv_obj_set_style_text_color(s_hint, theme->muted, 0);
    lv_label_set_text(s_hint, "轻触查看冰箱库存");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -88);
}

void fridge_ui_page_standby_update(void)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    char text[32] = {0};
    if (tm_now.tm_year >= 120) {
        strftime(text, sizeof(text), "%H:%M", &tm_now);
    } else {
        snprintf(text, sizeof(text), "--:--");
    }
    fridge_ui_label_set_text_if_changed(s_clock, text);

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_face_ms > 2600) {
        s_face_index = (s_face_index + 1) % (sizeof(FACES) / sizeof(FACES[0]));
        fridge_ui_label_set_text_if_changed(s_face, FACES[s_face_index]);
        s_last_face_ms = now_ms;
    }

    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    fridge_ui_label_set_text_fmt_if_changed(s_hint,
                                            "亮度 %u%%  雷达 %s",
                                            model.sensors.light_percent,
                                            model.sensors.radar_stable_presence ? "有人" : "无人");
}
