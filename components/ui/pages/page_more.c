// 冰箱小精灵更多功能页。
// 所有入口先可触摸，其中未完成的重业务入口给明确提示。

#include "fridge_ui_internal.h"

typedef struct {
    const char *text;
    fridge_ui_page_t page;
    bool navigate;
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

void fridge_ui_page_more_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    static more_item_t items[] = {
        {"AI 菜谱\n按库存生成", FRIDGE_UI_PAGE_RECIPE, true},
        {"购物清单\n采购建议", FRIDGE_UI_PAGE_SHOPPING, true},
        {"烹饪问答\n厨房知识库", FRIDGE_UI_PAGE_MORE, false},
        {"营养助手\n热量与忌口", FRIDGE_UI_PAGE_MORE, false},
        {"定时器\n煮蛋 8 分钟", FRIDGE_UI_PAGE_MORE, false},
        {"家庭同步\n多人共享", FRIDGE_UI_PAGE_MORE, false},
        {"离线模式\n本地可用", FRIDGE_UI_PAGE_OFFLINE, true},
        {"设备运维\nOTA / 日志", FRIDGE_UI_PAGE_SETTINGS, true},
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

    for (uint8_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        lv_obj_t *btn = lv_button_create(parent);
        lv_obj_set_size(btn, 300, 72);
        lv_obj_set_pos(btn, 42 + (i % 2) * 338, 98 + (i / 2) * 86);
        lv_obj_set_style_bg_color(btn, theme->surface, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_color(btn, theme->line, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, more_cb, LV_EVENT_CLICKED, &items[i]);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, items[i].text);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }
}

void fridge_ui_page_more_update(void)
{
}
