// 冰箱小精灵 AI 菜谱页。
// 先按 ui-reference 移植本地推荐菜单；真实 AI 生成后续通过后台任务写入模型，不能阻塞 LVGL 任务。

#include "fridge_ui_internal.h"

typedef struct {
    const char *tag;
    const char *title;
    const char *desc;
    lv_color_t tint;
    lv_color_t accent;
} recipe_item_t;

static const recipe_item_t RECIPES[] = {
    {
        .tag = "主菜 · 18 分钟",
        .title = "番茄炒蛋",
        .desc = "使用番茄、鸡蛋，顺手消耗临期库存。",
        .tint = LV_COLOR_MAKE(0xF8, 0xEC, 0xD4),
        .accent = LV_COLOR_MAKE(0xD9, 0x57, 0x45),
    },
    {
        .tag = "汤品 · 12 分钟",
        .title = "菠菜鸡蛋汤",
        .desc = "补充蔬菜和蛋白质，适合作为清淡汤品。",
        .tint = LV_COLOR_MAKE(0xE5, 0xF3, 0xDF),
        .accent = LV_COLOR_MAKE(0x4F, 0x8F, 0x62),
    },
    {
        .tag = "配菜 · 5 分钟",
        .title = "黄瓜酸奶小碟",
        .desc = "清爽解腻，顺手处理已开封酸奶。",
        .tint = LV_COLOR_MAKE(0xE6, 0xF2, 0xF3),
        .accent = LV_COLOR_MAKE(0x42, 0x86, 0x82),
    },
};

static void recipe_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_toast("已准备推送到手机小程序");
}

static lv_obj_t *create_summary_chip(lv_obj_t *parent, const char *text, int16_t x)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_pos(chip, x, 80);
    lv_obj_set_size(chip, 196, 34);
    lv_obj_set_style_bg_color(chip, theme->surface, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chip, theme->line, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_radius(chip, 17, 0);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, theme->accent, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_small(), 0);
    lv_obj_center(label);
    return chip;
}

static void create_dish_art(lv_obj_t *parent, const recipe_item_t *recipe, uint8_t index)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *photo = lv_obj_create(parent);
    lv_obj_remove_style_all(photo);
    lv_obj_set_pos(photo, 456, 30);
    lv_obj_set_size(photo, 142, 116);
    lv_obj_set_style_bg_color(photo, lv_color_hex(0xFFFADF), 0);
    lv_obj_set_style_bg_opa(photo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(photo, theme->line, 0);
    lv_obj_set_style_border_width(photo, 1, 0);
    lv_obj_set_style_radius(photo, 18, 0);

    // 用 LVGL 基础图形模拟参考稿里的菜品插画，避免引入额外位图占用 Flash。
    lv_obj_t *plate = lv_obj_create(photo);
    lv_obj_remove_style_all(plate);
    lv_obj_set_size(plate, 92, 64);
    lv_obj_set_pos(plate, 25, 20);
    lv_obj_set_style_bg_color(plate, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(plate, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(plate, index == 1 ? 20 : 32, 0);

    for (uint8_t i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_create(photo);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, i == 0 ? 26 : 22, i == 0 ? 26 : 22);
        lv_obj_set_pos(dot, 42 + i * 18, 42 + (i % 2) * 12);
        lv_obj_set_style_bg_color(dot, i % 2 ? recipe->accent : lv_color_hex(0xFFD058), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, 14, 0);
    }

    lv_obj_t *caption = lv_label_create(photo);
    lv_label_set_text(caption, index == 0 ? "推荐菜品" : (index == 1 ? "清淡汤品" : "快手配菜"));
    lv_obj_set_style_bg_color(caption, theme->accent, 0);
    lv_obj_set_style_bg_opa(caption, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(caption, lv_color_white(), 0);
    lv_obj_set_style_text_font(caption, fridge_ui_font_small(), 0);
    lv_obj_set_style_pad_left(caption, 14, 0);
    lv_obj_set_style_pad_right(caption, 14, 0);
    lv_obj_set_style_pad_top(caption, 4, 0);
    lv_obj_set_style_pad_bottom(caption, 4, 0);
    lv_obj_set_style_radius(caption, 12, 0);
    lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static lv_obj_t *create_recipe_card(lv_obj_t *parent, const recipe_item_t *recipe, uint8_t index)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();

    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_size(card, 660, 220);
    lv_obj_set_pos(card, 30, 132);
    lv_obj_set_style_bg_color(card, recipe->tint, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_color(card, theme->line, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_add_event_cb(card, recipe_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *tag = lv_label_create(card);
    lv_label_set_text(tag, recipe->tag);
    lv_obj_set_style_text_color(tag, recipe->accent, 0);
    lv_obj_set_style_text_font(tag, fridge_ui_font_small(), 0);
    lv_obj_set_pos(tag, 26, 30);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, recipe->title);
    lv_obj_set_style_text_color(title, theme->text, 0);
    lv_obj_set_style_text_font(title, fridge_ui_font_large(), 0);
    lv_obj_set_pos(title, 26, 68);
    lv_obj_set_width(title, 390);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    lv_obj_t *desc = lv_label_create(card);
    lv_label_set_text(desc, recipe->desc);
    lv_obj_set_style_text_color(desc, theme->muted, 0);
    lv_obj_set_style_text_font(desc, fridge_ui_font_small(), 0);
    lv_obj_set_pos(desc, 26, 116);
    lv_obj_set_width(desc, 390);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);

    create_dish_art(card, recipe, index);
    return card;
}

static void create_push_hint(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();

    lv_obj_t *hint = lv_obj_create(parent);
    lv_obj_remove_style_all(hint);
    lv_obj_set_size(hint, 276, 38);
    lv_obj_set_pos(hint, 414, 38);
    lv_obj_set_style_bg_color(hint, theme->surface, 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hint, 19, 0);
    lv_obj_set_style_border_color(hint, lv_color_hex(0xF0D9B5), 0);
    lv_obj_set_style_border_width(hint, 1, 0);

    // 提示放在标题右侧，真正的触发入口是下方菜谱主卡片。
    lv_obj_t *label = lv_label_create(hint);
    lv_label_set_text(label, "点击卡片推送到小程序");
    lv_obj_set_style_text_color(label, theme->accent_2, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_small(), 0);
    lv_obj_center(label);
}

void fridge_ui_page_recipe_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_bottom(parent, 108, 0);

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "厨房 AI 助手");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "今晚可以这样做");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    create_summary_chip(parent, "3 道菜", 30);
    create_summary_chip(parent, "约 35 分钟", 262);
    create_summary_chip(parent, "消耗 6 项库存", 494);

    create_recipe_card(parent, &RECIPES[0], 0);
    create_push_hint(parent);
}

void fridge_ui_page_recipe_update(void)
{
}
