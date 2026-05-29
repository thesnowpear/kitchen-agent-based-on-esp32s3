// 冰箱小精灵拍照登记页。
// 本轮以屏幕触摸验收为主，快门先生成可确认的本地候选，避免相机链路阻塞 LVGL。

#include "fridge_ui_internal.h"

static void shutter_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_CAMERA_RESULT);
}

void fridge_ui_page_camera_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "拍照登记");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 22);

    lv_obj_t *finder = lv_obj_create(parent);
    lv_obj_set_size(finder, 560, 340);
    lv_obj_align(finder, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_color(finder, lv_color_hex(0x2F6047), 0);
    lv_obj_set_style_radius(finder, 8, 0);
    lv_obj_set_style_border_color(finder, theme->line, 0);
    lv_obj_set_style_border_width(finder, 2, 0);

    lv_obj_t *hint = lv_label_create(finder);
    lv_label_set_text(hint, "取景框\n相机未接入时使用模拟识别");
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(hint);

    lv_obj_t *shutter = lv_button_create(parent);
    lv_obj_set_size(shutter, 104, 104);
    lv_obj_align(shutter, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_set_style_radius(shutter, 52, 0);
    lv_obj_set_style_bg_color(shutter, theme->accent_2, 0);
    lv_obj_add_event_cb(shutter, shutter_cb, LV_EVENT_CLICKED, NULL);
}

void fridge_ui_page_camera_update(void)
{
}
