// 冰箱小精灵开门临期提醒页。
// 当前从本地库存中筛出临期项展示，真实开门状态机后续再接入。

#include "fridge_ui_internal.h"

static lv_obj_t *s_rows[4];
static lv_obj_t *s_labels[4];

static void row_cb(lv_event_t *event)
{
    uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    size_t visible = 0;
    for (size_t i = 0; i < model.food_count; i++) {
        if (model.foods[i].days_left <= 3) {
            if (visible == index) {
                fridge_ui_model_select_cell(model.foods[i].zone, model.foods[i].cell);
                fridge_ui_show_page(FRIDGE_UI_PAGE_EDIT_FOOD);
                return;
            }
            visible++;
        }
    }
}

void fridge_ui_page_door_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "今天优先吃什么");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 22);

    for (uint8_t i = 0; i < 4; i++) {
        s_rows[i] = lv_button_create(parent);
        lv_obj_set_size(s_rows[i], 640, 82);
        lv_obj_set_pos(s_rows[i], 40, 88 + i * 96);
        lv_obj_set_style_bg_color(s_rows[i], theme->surface, 0);
        lv_obj_set_style_radius(s_rows[i], 8, 0);
        lv_obj_set_style_shadow_width(s_rows[i], 0, 0);
        lv_obj_add_event_cb(s_rows[i], row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_labels[i] = lv_label_create(s_rows[i]);
        lv_obj_align(s_labels[i], LV_ALIGN_LEFT_MID, 16, 0);
    }
}

void fridge_ui_page_door_update(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    uint8_t row = 0;
    for (size_t i = 0; i < model.food_count && row < 4; i++) {
        const fridge_ui_food_t *food = &model.foods[i];
        if (food->days_left > 3) {
            continue;
        }
        lv_obj_clear_flag(s_rows[row], LV_OBJ_FLAG_HIDDEN);
        fridge_ui_label_set_text_fmt_if_changed(s_labels[row], "%s · %s\n%s · %d 天内", food->name, food->quantity, food->location, food->days_left);
        row++;
    }
    for (; row < 4; row++) {
        lv_obj_add_flag(s_rows[row], LV_OBJ_FLAG_HIDDEN);
    }
}
