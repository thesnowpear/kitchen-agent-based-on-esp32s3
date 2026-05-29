// 冰箱小精灵区域九宫格。
// 用户可在九宫格中增删食材；自定义空间支持重命名和删除。

#include "fridge_ui_internal.h"

static lv_obj_t *s_title;
static lv_obj_t *s_rename_btn;
static lv_obj_t *s_delete_btn;
static lv_obj_t *s_cells[FRIDGE_UI_ZONE_CELL_COUNT];
static lv_obj_t *s_cell_labels[FRIDGE_UI_ZONE_CELL_COUNT];

static void rename_done_cb(const char *text)
{
    if (fridge_ui_model_rename_active_zone(text)) {
        fridge_ui_toast("已重命名空间");
        fridge_ui_page_zone_update();
    } else {
        fridge_ui_toast("只能重命名自定义空间");
    }
}

static void cell_cb(lv_event_t *event)
{
    uintptr_t cell = (uintptr_t)lv_event_get_user_data(event);
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    if (fridge_ui_model_is_place_picking()) {
        if (fridge_ui_model_apply_place_pick(model.active_zone, (uint8_t)cell)) {
            fridge_ui_toast("位置已更新");
            fridge_ui_show_page(FRIDGE_UI_PAGE_EDIT_FOOD);
        }
        return;
    }
    fridge_ui_model_select_cell(model.active_zone, (uint8_t)cell);
    fridge_ui_show_page(FRIDGE_UI_PAGE_EDIT_FOOD);
}

static void back_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
}

static void rename_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    fridge_ui_keyboard_open_text("空间名称", model.zones[model.active_zone].name, rename_done_cb);
}

static void delete_space_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_model_delete_active_custom_zone();
    fridge_ui_toast("已删除空间");
    fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
}

static lv_obj_t *make_header_button(lv_obj_t *parent, int16_t x, int16_t w, const char *text, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, 18);
    lv_obj_set_size(btn, w, 46);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    return btn;
}

void fridge_ui_page_zone_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *back = lv_button_create(parent);
    lv_obj_set_size(back, 86, 46);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 28, 18);
    lv_obj_set_style_bg_color(back, theme->surface_soft, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "返回");
    lv_obj_center(back_label);

    s_title = lv_label_create(parent);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 24);

    s_rename_btn = make_header_button(parent, 468, 92, "重命名", theme->accent, rename_cb);
    s_delete_btn = make_header_button(parent, 574, 112, "删除空间", theme->danger, delete_space_cb);

    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_pos(grid, 34, 90);
    lv_obj_set_size(grid, 652, 410);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER);

    for (uint8_t i = 0; i < FRIDGE_UI_ZONE_CELL_COUNT; i++) {
        lv_obj_t *cell = lv_button_create(grid);
        lv_obj_set_size(cell, 205, 125);
        lv_obj_set_style_radius(cell, 8, 0);
        lv_obj_set_style_shadow_width(cell, 0, 0);
        lv_obj_set_style_bg_color(cell, theme->surface, 0);
        lv_obj_set_style_border_color(cell, theme->line, 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_add_event_cb(cell, cell_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_cells[i] = cell;
        s_cell_labels[i] = lv_label_create(cell);
        lv_label_set_text(s_cell_labels[i], "-");
        lv_obj_set_style_text_align(s_cell_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_cell_labels[i]);
    }
}

void fridge_ui_page_zone_update(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    bool picking = fridge_ui_model_is_place_picking();
    fridge_ui_label_set_text_fmt_if_changed(s_title, picking ? "选择新位置 · %s" : "%s 九宫格", model.zones[model.active_zone].name);
    bool custom = model.active_zone >= 4 && model.active_zone < FRIDGE_UI_ZONE_COUNT && model.zones[model.active_zone].custom;
    if (s_rename_btn) {
        if (custom) {
            lv_obj_remove_flag(s_rename_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_rename_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_delete_btn) {
        if (custom) {
            lv_obj_remove_flag(s_delete_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_delete_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    const char *cells[] = {"A1", "A2", "A3", "B1", "B2", "B3", "C1", "C2", "C3"};
    for (uint8_t i = 0; i < FRIDGE_UI_ZONE_CELL_COUNT; i++) {
        fridge_ui_label_set_text_fmt_if_changed(s_cell_labels[i], "%s\n空", cells[i]);
    }
    for (size_t i = 0; i < model.food_count; i++) {
        const fridge_ui_food_t *food = &model.foods[i];
        if (food->zone == model.active_zone && food->cell < FRIDGE_UI_ZONE_CELL_COUNT) {
            fridge_ui_label_set_text_fmt_if_changed(s_cell_labels[food->cell],
                                                    picking ? "%s\n%s\n点此放入" : "%s\n%s\n%d 天",
                                                    food->name,
                                                    food->quantity,
                                                    food->days_left);
        }
    }
}
