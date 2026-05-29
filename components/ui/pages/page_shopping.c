// 冰箱小精灵购物清单页。
// 当前展示根据本地库存推导的静态建议，checkbox 只做屏幕交互反馈。

#include "fridge_ui_internal.h"

void fridge_ui_page_shopping_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "自动购物清单");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "下次采购建议");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    const char *items[] = {"酸奶 2 杯 · 低库存", "生菜 1 颗 · 搭配菜单", "鸡胸肉 1 份 · 常购补货", "牛奶 1 盒 · 临期后补"};
    for (uint8_t i = 0; i < 4; i++) {
        lv_obj_t *cb = lv_checkbox_create(parent);
        lv_checkbox_set_text(cb, items[i]);
        lv_obj_set_size(cb, 610, 58);
        lv_obj_set_pos(cb, 54, 110 + i * 78);
        lv_obj_set_style_text_color(cb, theme->text, 0);
        lv_obj_set_style_text_font(cb, fridge_ui_font_body(), 0);
        if (i == 3) {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        }
    }

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, "已勾选的条目会保留在本地清单，后续接入家庭同步。");
    lv_obj_set_width(hint, 620);
    lv_obj_set_style_text_color(hint, theme->muted, 0);
    lv_obj_set_style_text_font(hint, fridge_ui_font_small(), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 50, -26);
}

void fridge_ui_page_shopping_update(void)
{
}
