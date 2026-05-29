// 冰箱小精灵区域九宫格。
// 用户可在九宫格中增删食材；自定义空间支持重命名和删除。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *s_title;
static lv_obj_t *s_kicker;
static lv_obj_t *s_board;
static lv_obj_t *s_place_hint;
static lv_obj_t *s_rename_btn;
static lv_obj_t *s_delete_btn;
static lv_obj_t *s_cells[FRIDGE_UI_ZONE_CELL_COUNT];
static lv_obj_t *s_cell_codes[FRIDGE_UI_ZONE_CELL_COUNT];
static lv_obj_t *s_cell_foods[FRIDGE_UI_ZONE_CELL_COUNT];
static lv_obj_t *s_cell_amounts[FRIDGE_UI_ZONE_CELL_COUNT];
static lv_obj_t *s_cell_expires[FRIDGE_UI_ZONE_CELL_COUNT];

static const char *CELL_CODES[FRIDGE_UI_ZONE_CELL_COUNT] = {"内 · 左", "内 · 中", "内 · 右",
                                                             "中 · 左", "中 · 中", "中 · 右",
                                                             "外 · 左", "外 · 中", "外 · 右"};

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
    lv_obj_set_pos(btn, x, 24);
    lv_obj_set_size(btn, w, 40);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_small(), 0);
    lv_obj_center(label);
    return btn;
}

static void set_cell_selected(uint8_t cell, bool selected, bool picking)
{
    if (cell >= FRIDGE_UI_ZONE_CELL_COUNT || !s_cells[cell]) {
        return;
    }
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_style_bg_opa(s_cells[cell], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s_cells[cell], selected ? lv_color_hex(0xFFF5DC) : theme->surface, 0);
    lv_obj_set_style_border_width(s_cells[cell], selected || picking ? 3 : 0, 0);
    lv_obj_set_style_border_color(s_cells[cell], selected ? theme->accent_2 : theme->accent, 0);
}

static void set_label_font_for_text(lv_obj_t *label, const lv_font_t *primary, const char *text)
{
    if (!label) {
        return;
    }
    lv_obj_set_style_text_font(label, fridge_ui_font_for_text(primary, text), 0);
}

static void set_cell_text(uint8_t cell, const char *food, const char *amount, const char *expire)
{
    if (cell >= FRIDGE_UI_ZONE_CELL_COUNT) {
        return;
    }
    const char *food_text = food && food[0] ? food : "空位";
    const char *amount_text = amount && amount[0] ? amount : "待设置";
    char expire_text[32] = {0};
    snprintf(expire_text, sizeof(expire_text), "到期 %s", expire && expire[0] ? expire : "待设置");

    set_label_font_for_text(s_cell_foods[cell], fridge_ui_font_body(), food_text);
    set_label_font_for_text(s_cell_amounts[cell], fridge_ui_font_small(), amount_text);
    set_label_font_for_text(s_cell_expires[cell], fridge_ui_font_small(), expire_text);
    fridge_ui_label_set_text_if_changed(s_cell_foods[cell], food_text);
    fridge_ui_label_set_text_if_changed(s_cell_amounts[cell], amount_text);
    fridge_ui_label_set_text_if_changed(s_cell_expires[cell], expire_text);
}

void fridge_ui_page_zone_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    (void)back_cb;
    s_kicker = lv_label_create(parent);
    lv_label_set_text(s_kicker, "九宫格位置管理");
    lv_obj_set_style_text_color(s_kicker, theme->accent, 0);
    lv_obj_set_style_text_font(s_kicker, fridge_ui_font_small(), 0);
    lv_obj_align(s_kicker, LV_ALIGN_TOP_LEFT, 34, 16);

    s_title = lv_label_create(parent);
    lv_obj_set_style_text_color(s_title, theme->text, 0);
    lv_obj_set_style_text_font(s_title, fridge_ui_font_title(), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 34, 38);

    s_rename_btn = make_header_button(parent, 484, 82, "重命名", theme->accent, rename_cb);
    s_delete_btn = make_header_button(parent, 578, 108, "删除空间", theme->danger, delete_space_cb);

    s_place_hint = lv_label_create(parent);
    lv_label_set_text(s_place_hint, "位置编辑模式：请选择新的九宫格位置");
    lv_obj_set_pos(s_place_hint, 34, 78);
    lv_obj_set_size(s_place_hint, 652, 42);
    lv_obj_set_style_bg_color(s_place_hint, theme->surface_panel, 0);
    lv_obj_set_style_bg_opa(s_place_hint, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_place_hint, lv_color_hex(0xBFD4BA), 0);
    lv_obj_set_style_border_width(s_place_hint, 1, 0);
    lv_obj_set_style_radius(s_place_hint, 16, 0);
    lv_obj_set_style_pad_left(s_place_hint, 14, 0);
    lv_obj_set_style_pad_top(s_place_hint, 10, 0);
    lv_obj_set_style_text_color(s_place_hint, theme->accent, 0);
    lv_obj_set_style_text_font(s_place_hint, fridge_ui_font_small(), 0);

    s_board = lv_obj_create(parent);
    lv_obj_remove_style_all(s_board);
    lv_obj_set_pos(s_board, 34, 88);
    lv_obj_set_size(s_board, 652, 414);
    lv_obj_remove_flag(s_board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_board, lv_color_hex(0xFFFEFA), 0);
    lv_obj_set_style_bg_opa(s_board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_board, theme->line, 0);
    lv_obj_set_style_border_width(s_board, 2, 0);
    lv_obj_set_style_radius(s_board, 24, 0);
    lv_obj_set_style_clip_corner(s_board, true, 0);

    for (uint8_t i = 0; i < FRIDGE_UI_ZONE_CELL_COUNT; i++) {
        uint8_t row = i / 3;
        uint8_t col = i % 3;
        int16_t x = (int16_t)(col * 217);
        int16_t y = (int16_t)(row * 138);
        int16_t w = (col == 2) ? 218 : 217;
        int16_t h = (row == 2) ? 138 : 138;

        lv_obj_t *cell = lv_button_create(s_board);
        lv_obj_set_pos(cell, x, y);
        lv_obj_set_size(cell, w, h);
        lv_obj_set_style_radius(cell, 0, 0);
        lv_obj_set_style_shadow_width(cell, 0, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 10, 0);
        lv_obj_add_event_cb(cell, cell_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_cells[i] = cell;

        if (col < 2) {
            lv_obj_t *line = lv_obj_create(s_board);
            lv_obj_remove_style_all(line);
            lv_obj_set_pos(line, x + w - 1, y);
            lv_obj_set_size(line, 1, h);
            lv_obj_set_style_bg_color(line, theme->line, 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        }
        if (row < 2) {
            lv_obj_t *line = lv_obj_create(s_board);
            lv_obj_remove_style_all(line);
            lv_obj_set_pos(line, x, y + h - 1);
            lv_obj_set_size(line, w, 1);
            lv_obj_set_style_bg_color(line, theme->line, 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        }

        s_cell_codes[i] = lv_label_create(cell);
        lv_label_set_text(s_cell_codes[i], CELL_CODES[i]);
        lv_obj_set_style_text_color(s_cell_codes[i], theme->accent, 0);
        lv_obj_set_style_text_font(s_cell_codes[i], fridge_ui_font_small(), 0);
        lv_obj_align(s_cell_codes[i], LV_ALIGN_TOP_LEFT, 0, 0);

        s_cell_foods[i] = lv_label_create(cell);
        lv_label_set_long_mode(s_cell_foods[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_cell_foods[i], 100);
        lv_obj_set_style_text_color(s_cell_foods[i], theme->text, 0);
        lv_obj_set_style_text_font(s_cell_foods[i], fridge_ui_font_body(), 0);
        lv_obj_align(s_cell_foods[i], LV_ALIGN_CENTER, -48, -4);

        s_cell_amounts[i] = lv_label_create(cell);
        lv_label_set_long_mode(s_cell_amounts[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_cell_amounts[i], 76);
        lv_obj_set_style_text_align(s_cell_amounts[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(s_cell_amounts[i], theme->accent, 0);
        lv_obj_set_style_text_font(s_cell_amounts[i], fridge_ui_font_small(), 0);
        lv_obj_align(s_cell_amounts[i], LV_ALIGN_CENTER, 50, -4);

        s_cell_expires[i] = lv_label_create(cell);
        lv_label_set_long_mode(s_cell_expires[i], LV_LABEL_LONG_DOT);
        lv_obj_set_size(s_cell_expires[i], w - 20, 28);
        lv_obj_set_style_bg_color(s_cell_expires[i], lv_color_hex(0xFFF0D0), 0);
        lv_obj_set_style_bg_opa(s_cell_expires[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_cell_expires[i], 14, 0);
        lv_obj_set_style_pad_top(s_cell_expires[i], 5, 0);
        lv_obj_set_style_text_align(s_cell_expires[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_cell_expires[i], lv_color_hex(0x8A5D00), 0);
        lv_obj_set_style_text_font(s_cell_expires[i], fridge_ui_font_small(), 0);
        lv_obj_align(s_cell_expires[i], LV_ALIGN_BOTTOM_MID, 0, 0);
    }
}

void fridge_ui_page_zone_update(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    bool picking = fridge_ui_model_is_place_picking();
    fridge_ui_label_set_text_if_changed(s_kicker, picking ? "九宫格位置选择" : "九宫格位置管理");
    fridge_ui_label_set_text_if_changed(s_title, model.zones[model.active_zone].name);
    if (s_place_hint) {
        if (picking) {
            lv_obj_remove_flag(s_place_hint, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(s_board, 124);
            lv_obj_set_height(s_board, 414);
        } else {
            lv_obj_add_flag(s_place_hint, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(s_board, 88);
            lv_obj_set_height(s_board, 414);
        }
    }
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
    for (uint8_t i = 0; i < FRIDGE_UI_ZONE_CELL_COUNT; i++) {
        set_cell_selected(i, i == model.active_cell, picking);
        set_cell_text(i, "空位", "可登记", "待设置");
    }
    for (size_t i = 0; i < model.food_count; i++) {
        const fridge_ui_food_t *food = &model.foods[i];
        if (food->zone == model.active_zone && food->cell < FRIDGE_UI_ZONE_CELL_COUNT) {
            char expire[24] = {0};
            if (picking) {
                strlcpy(expire, "点此放入", sizeof(expire));
            } else if (food->days_left <= 0) {
                strlcpy(expire, "今天", sizeof(expire));
            } else if (food->days_left == 1) {
                strlcpy(expire, "明天", sizeof(expire));
            } else {
                snprintf(expire, sizeof(expire), "%d 天后", food->days_left);
            }
            set_cell_text(food->cell, food->name, food->quantity, expire);
        }
    }
}
