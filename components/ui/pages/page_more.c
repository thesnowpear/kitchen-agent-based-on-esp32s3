// 冰箱小精灵更多功能页。
// 对齐 ui-reference 的“扩展厨房能力”网格；未接入的重业务入口只给轻提示，不阻塞 LVGL 任务。

#include "fridge_ui_internal.h"

typedef struct {
    const char *title;
    const char *subtitle;
    fridge_ui_page_t page;
    bool navigate;
    lv_color_t tint;
} more_item_t;

static void more_cb(lv_event_t *event)
{
    more_item_t *item = lv_event_get_user_data(event);
    if (!item) {
        return;
    }
    if (item->navigate) {
        fridge_ui_show_page(item->page);
    } else {
        fridge_ui_toast("该功能后续接入");
    }
}

static void make_card_child_passive(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }

    // 卡片内部的装饰层只负责绘制，不能抢走触摸命中；点击统一交给整张卡片按钮处理。
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static void create_more_card(lv_obj_t *parent, more_item_t *item, uint8_t index)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    const int16_t col = index % 2;
    const int16_t row = index / 2;
    const lv_color_t card_bg = lv_color_mix(item->tint, theme->surface, LV_OPA_10);
    const lv_color_t soft_tint = lv_color_mix(item->tint, theme->surface, LV_OPA_20);
    const lv_color_t faint_tint = lv_color_mix(item->tint, theme->surface, LV_OPA_10);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 324, 96);
    lv_obj_set_pos(btn, 38 + col * 344, 108 + row * 116);
    lv_obj_set_style_bg_color(btn, card_bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_border_color(btn, lv_color_mix(item->tint, theme->line, LV_OPA_30), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 10, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_10, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x8A9A84), 0);
    lv_obj_set_style_shadow_offset_y(btn, 4, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, more_cb, LV_EVENT_CLICKED, item);

    // 左侧色条和右上角柔和块让白卡有层次；文字再放入居中内容组，避免实屏看起来偏下。
    lv_obj_t *mark = lv_obj_create(btn);
    lv_obj_remove_style_all(mark);
    lv_obj_set_size(mark, 7, 48);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_color(mark, item->tint, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mark, 4, 0);
    make_card_child_passive(mark);

    lv_obj_t *corner = lv_obj_create(btn);
    lv_obj_remove_style_all(corner);
    lv_obj_set_size(corner, 72, 48);
    lv_obj_align(corner, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(corner, soft_tint, 0);
    lv_obj_set_style_bg_opa(corner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(corner, 18, 0);
    make_card_child_passive(corner);

    lv_obj_t *dot = lv_obj_create(btn);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 18, 18);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -22, 20);
    lv_obj_set_style_bg_color(dot, item->tint, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_50, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    make_card_child_passive(dot);

    lv_obj_t *content = lv_obj_create(btn);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 216, 62);
    lv_obj_align(content, LV_ALIGN_LEFT_MID, 42, -1);
    make_card_child_passive(content);

    lv_obj_t *rule = lv_obj_create(btn);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 196, 1);
    lv_obj_set_pos(rule, 0, 37);
    lv_obj_set_style_bg_color(rule, faint_tint, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    make_card_child_passive(rule);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, item->title);
    lv_obj_set_style_text_color(title, theme->text, 0);
    lv_obj_set_style_text_font(title, fridge_ui_font_large(), 0);
    lv_obj_set_pos(title, 0, -3);
    lv_obj_set_width(title, 216);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    lv_obj_t *subtitle = lv_label_create(content);
    lv_label_set_text(subtitle, item->subtitle);
    lv_obj_set_style_text_color(subtitle, theme->muted, 0);
    lv_obj_set_style_text_font(subtitle, fridge_ui_font_small(), 0);
    lv_obj_set_pos(subtitle, 0, 42);
    lv_obj_set_width(subtitle, 216);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
}

void fridge_ui_page_more_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_bottom(parent, 92, 0);

    static more_item_t items[] = {
        {"AI 菜谱", "按库存生成", FRIDGE_UI_PAGE_RECIPE, true, LV_COLOR_MAKE(0xD9, 0x57, 0x45)},
        {"营养助手", "热量与忌口", FRIDGE_UI_PAGE_NUTRITION, true, LV_COLOR_MAKE(0x6B, 0x8F, 0x71)},
        {"定时器", "煮蛋 8 分钟", FRIDGE_UI_PAGE_TIMER, true, LV_COLOR_MAKE(0xE0, 0xA9, 0x41)},
        {"烹饪问答", "厨房知识库", FRIDGE_UI_PAGE_MORE, false, LV_COLOR_MAKE(0x4E, 0x82, 0xA6)},
        {"秒表", "烹饪计时", FRIDGE_UI_PAGE_STOPWATCH, true, LV_COLOR_MAKE(0x70, 0x6A, 0xB8)},
        {"闹钟", "定时提醒", FRIDGE_UI_PAGE_ALARM, true, LV_COLOR_MAKE(0xB7, 0x6A, 0x72)},
        {"购物清单", "采购建议", FRIDGE_UI_PAGE_SHOPPING, true, LV_COLOR_MAKE(0x43, 0x8C, 0x74)},
        {"离线模式", "本地可用", FRIDGE_UI_PAGE_OFFLINE, true, LV_COLOR_MAKE(0x8C, 0x7A, 0x43)},
    };

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "更多功能");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "扩展厨房能力");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, "把常用厨房动作集中到一屏，已接入的入口可直接打开。");
    lv_obj_set_style_text_color(hint, theme->muted, 0);
    lv_obj_set_style_text_font(hint, fridge_ui_font_small(), 0);
    lv_obj_set_pos(hint, 38, 78);
    lv_obj_set_width(hint, 640);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);

    for (uint8_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        create_more_card(parent, &items[i], i);
    }
}

void fridge_ui_page_more_update(void)
{
}
