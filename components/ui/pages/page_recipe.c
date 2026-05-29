// 冰箱小精灵 AI 菜谱页。
// 本轮先展示本地推荐菜单；真实 AI 生成后续通过 worker 任务异步接入，不能阻塞 LVGL 任务。

#include "fridge_ui_internal.h"

static lv_obj_t *s_detail_labels[3];
static bool s_expanded[3];

static const char *DETAILS[] = {
    "鸡蛋 2 个打散，番茄切块。先炒鸡蛋再炒番茄出汁，少盐调味。",
    "水开后加入菠菜，淋入半个蛋液，少量盐调味即可。",
    "黄瓜切片，酸奶加黑胡椒或蜂蜜，作为清爽冷盘。",
};

static void recipe_cb(lv_event_t *event)
{
    uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index >= 3 || !s_detail_labels[index]) {
        return;
    }
    s_expanded[index] = !s_expanded[index];
    if (s_expanded[index]) {
        lv_obj_clear_flag(s_detail_labels[index], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_detail_labels[index], LV_OBJ_FLAG_HIDDEN);
    }
}

void fridge_ui_page_recipe_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "厨房 AI 助手");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "今晚一顿餐");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    const char *recipes[] = {
        "番茄炒蛋\n主菜 · 18 分钟 · 消耗番茄和鸡蛋\n点击展开做法",
        "菠菜鸡蛋汤\n汤品 · 12 分钟 · 清淡汤品\n点击展开做法",
        "黄瓜酸奶小碟\n配菜 · 5 分钟 · 清爽配菜\n点击展开做法",
    };
    for (uint8_t i = 0; i < 3; i++) {
        lv_obj_t *card = lv_button_create(parent);
        lv_obj_set_size(card, 640, 112);
        lv_obj_set_pos(card, 40, 98 + i * 128);
        lv_obj_set_style_bg_color(card, theme->surface, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_border_color(card, theme->line, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_add_event_cb(card, recipe_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *label = lv_label_create(card);
        lv_label_set_text(label, recipes[i]);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 18, 0);

        s_detail_labels[i] = lv_label_create(parent);
        lv_label_set_text(s_detail_labels[i], DETAILS[i]);
        lv_obj_set_width(s_detail_labels[i], 610);
        lv_obj_set_style_text_color(s_detail_labels[i], theme->muted, 0);
        lv_obj_set_style_text_font(s_detail_labels[i], fridge_ui_font_small(), 0);
        lv_obj_set_pos(s_detail_labels[i], 58, 204 + i * 128);
        lv_obj_add_flag(s_detail_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void fridge_ui_page_recipe_update(void)
{
}
