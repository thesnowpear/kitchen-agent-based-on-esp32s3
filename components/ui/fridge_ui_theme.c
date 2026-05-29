// 冰箱小精灵 UI warm 主题。
// 当前实屏只有 72mm 见方，中文需要比 Web 原型更“重”一些；主题里集中管理颜色和字体层级。

#include "fridge_ui_internal.h"

LV_FONT_DECLARE(fridge_font_cn_16);
LV_FONT_DECLARE(fridge_font_cn_24);
LV_FONT_DECLARE(fridge_font_cn_32);
LV_FONT_DECLARE(fridge_font_cn_ai_16);
LV_FONT_DECLARE(fridge_font_digits_96);

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

const lv_font_t *fridge_ui_font_number(void)
{
    // 厨房工具页的主时间只显示 ASCII 数字和冒号，使用只含 0-9 和冒号的 96px 字体，观感清晰且体积很小。
    return &fridge_font_digits_96;
}

const lv_font_t *fridge_ui_font_ai_body(void)
{
    // AI 回复是开放文本，单独使用 GB2312 一级常用汉字小字号字库，避免把全局大字号字库撑爆。
    return &fridge_font_cn_ai_16;
}

// UTF-8 文本缺字检测。
// 注意：这里只用于 UI 文本选择字体，不修改文本内容；遇到主字体缺字时整段切到更全的小字库，避免词语中夹杂方框。
static bool ui_next_utf8_codepoint(const char **cursor, uint32_t *codepoint)
{
    if (!cursor || !*cursor || !codepoint) {
        return false;
    }
    const unsigned char *p = (const unsigned char *)*cursor;
    if (*p == '\0') {
        return false;
    }

    if (*p < 0x80) {
        *codepoint = *p;
        *cursor += 1;
        return true;
    }

    uint32_t cp = 0;
    uint8_t need = 0;
    if ((*p & 0xE0) == 0xC0) {
        cp = *p & 0x1F;
        need = 2;
    } else if ((*p & 0xF0) == 0xE0) {
        cp = *p & 0x0F;
        need = 3;
    } else if ((*p & 0xF8) == 0xF0) {
        cp = *p & 0x07;
        need = 4;
    } else {
        return false;
    }

    for (uint8_t i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return false;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *codepoint = cp;
    *cursor += need;
    return true;
}

bool fridge_ui_font_covers_text(const lv_font_t *font, const char *text)
{
    if (!font || !text) {
        return false;
    }

    const char *cursor = text;
    while (*cursor) {
        uint32_t cp = 0;
        if (!ui_next_utf8_codepoint(&cursor, &cp)) {
            return false;
        }
        if (cp == '\n' || cp == '\r' || cp == '\t') {
            continue;
        }

        lv_font_glyph_dsc_t glyph = {0};
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, 0) || glyph.resolved_font == NULL || glyph.is_placeholder) {
            return false;
        }
    }
    return true;
}

const lv_font_t *fridge_ui_font_for_text(const lv_font_t *primary, const char *text)
{
    if (fridge_ui_font_covers_text(primary, text)) {
        return primary;
    }
    return fridge_ui_font_ai_body();
}
