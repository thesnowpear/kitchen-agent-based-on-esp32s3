// 冰箱小精灵 UI warm 主题。
// 当前实屏只有 72mm 见方，中文需要比 Web 原型更“重”一些；主题里集中管理颜色和字体层级。

#include "fridge_ui_internal.h"

LV_FONT_DECLARE(fridge_font_cn_16);
LV_FONT_DECLARE(fridge_font_cn_24);
LV_FONT_DECLARE(fridge_font_cn_32);

static const fridge_ui_theme_t s_theme = {
    .bg = LV_COLOR_MAKE(0xFF, 0xFA, 0xEF),
    .surface = LV_COLOR_MAKE(0xFF, 0xFE, 0xFA),
    .surface_soft = LV_COLOR_MAKE(0xF0, 0xF6, 0xEC),
    .surface_panel = LV_COLOR_MAKE(0xEA, 0xF2, 0xE7),
    .text = LV_COLOR_MAKE(0x24, 0x34, 0x2D),
    .muted = LV_COLOR_MAKE(0x75, 0x80, 0x78),
    .accent = LV_COLOR_MAKE(0x2F, 0x60, 0x47),
    .accent_2 = LV_COLOR_MAKE(0xD9, 0x57, 0x45),
    .danger = LV_COLOR_MAKE(0xB8, 0x3B, 0x3B),
    .line = LV_COLOR_MAKE(0xD8, 0xE1, 0xD5),
};

const fridge_ui_theme_t *fridge_ui_theme_get(void)
{
    return &s_theme;
}

void fridge_ui_theme_apply(lv_obj_t *root)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_style_bg_color(root, theme->bg, 0);
    lv_obj_set_style_text_color(root, theme->text, 0);
    lv_obj_set_style_text_font(root, &fridge_font_cn_24, 0);
}

const lv_font_t *fridge_ui_font_small(void)
{
    return &fridge_font_cn_16;
}

const lv_font_t *fridge_ui_font_body(void)
{
    return &fridge_font_cn_24;
}

const lv_font_t *fridge_ui_font_large(void)
{
    return &fridge_font_cn_32;
}

const lv_font_t *fridge_ui_font_title(void)
{
    return &fridge_font_cn_32;
}
