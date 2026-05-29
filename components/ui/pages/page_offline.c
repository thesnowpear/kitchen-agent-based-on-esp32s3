// 冰箱小精灵离线页。
// 展示断网时仍可用的本地能力；真实重连状态从 network model 读取。

#include "fridge_ui_internal.h"

static lv_obj_t *s_status;

void fridge_ui_page_offline_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "离线也能继续提醒");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 22);

    s_status = lv_label_create(parent);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 46, 86);

    const char *items[] = {"本地库存\n已保存库存快照", "开门提醒\n临期列表可本地展示", "重连状态\n联网后恢复 AI 能力"};
    for (uint8_t i = 0; i < 3; i++) {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_set_size(card, 620, 92);
        lv_obj_set_pos(card, 50, 150 + i * 110);
        lv_obj_set_style_bg_color(card, theme->surface, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_border_color(card, theme->line, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_t *label = lv_label_create(card);
        lv_label_set_text(label, items[i]);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 18, 0);
    }
}

void fridge_ui_page_offline_update(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    fridge_ui_label_set_text_fmt_if_changed(s_status, "网络：%s", model.network.connected ? "已恢复" : "离线/重连中");
}
