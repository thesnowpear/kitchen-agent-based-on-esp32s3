// 冰箱小精灵开门临期提醒页。
// 该页对齐 ui-reference 的提醒卡片结构：左侧图形标记、中间主副文案、右侧编辑入口。
// 这里只做 LVGL 结构与样式更新，不改动任何硬件初始化、GPIO、时钟或状态机判定逻辑。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <string.h>

#define DOOR_MAX_ROWS 3

typedef enum {
    DOOR_MARK_EGG = 0,
    DOOR_MARK_MILK,
    DOOR_MARK_LEAF,
    DOOR_MARK_COUNT,
} door_mark_t;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *mark_wrap;
    lv_obj_t *mark_shape;
    lv_obj_t *title;
    lv_obj_t *meta;
    lv_obj_t *edit_btn;
} door_row_view_t;

static lv_obj_t *s_kicker;
static lv_obj_t *s_title;
static lv_obj_t *s_empty_card;
static lv_obj_t *s_empty_title;
static lv_obj_t *s_empty_meta;
static door_row_view_t s_rows[DOOR_MAX_ROWS];

static door_mark_t pick_mark_type(const fridge_ui_food_t *food)
{
    if (!food) {
        return DOOR_MARK_EGG;
    }

    if (strstr(food->name, "蛋")) {
        return DOOR_MARK_EGG;
    }
    if (strstr(food->name, "奶")) {
        return DOOR_MARK_MILK;
    }
    return DOOR_MARK_LEAF;
}

static const char *format_days_left(const fridge_ui_food_t *food, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return "";
    }
    if (!food) {
        strlcpy(buf, "待确认", buf_size);
        return buf;
    }
    if (food->days_left <= 0) {
        strlcpy(buf, "今天到期", buf_size);
    } else if (food->days_left == 1) {
        strlcpy(buf, "明天到期", buf_size);
    } else {
        snprintf(buf, buf_size, "%d天后到期", food->days_left);
    }
    return buf;
}

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

static void apply_mark_style(door_row_view_t *row, door_mark_t mark)
{
    if (!row || !row->mark_wrap || !row->mark_shape) {
        return;
    }

    const fridge_ui_theme_t *theme = fridge_ui_theme_get();

    lv_obj_set_style_radius(row->mark_wrap, 18, 0);
    lv_obj_set_style_bg_opa(row->mark_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(row->mark_wrap, 0, 0);
    lv_obj_set_style_border_width(row->mark_wrap, 0, 0);
    lv_obj_set_style_bg_color(row->mark_wrap, lv_color_hex(0xFFF4DC), 0);

    lv_obj_set_style_bg_opa(row->mark_shape, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(row->mark_shape, 0, 0);
    lv_obj_set_style_border_width(row->mark_shape, 0, 0);

    switch (mark) {
    case DOOR_MARK_EGG:
        lv_obj_set_size(row->mark_shape, 22, 22);
        lv_obj_center(row->mark_shape);
        lv_obj_set_style_radius(row->mark_shape, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(row->mark_shape, lv_color_hex(0xF6BF4C), 0);
        lv_obj_set_style_bg_color(row->mark_wrap, lv_color_hex(0xFFF4DC), 0);
        break;
    case DOOR_MARK_MILK:
        lv_obj_set_size(row->mark_shape, 34, 46);
        lv_obj_center(row->mark_shape);
        lv_obj_set_style_radius(row->mark_shape, 14, 0);
        lv_obj_set_style_bg_color(row->mark_shape, lv_color_hex(0xA6CEE8), 0);
        lv_obj_set_style_bg_color(row->mark_wrap, lv_color_hex(0xF1F7FE), 0);
        break;
    case DOOR_MARK_LEAF:
    default:
        lv_obj_set_size(row->mark_shape, 42, 24);
        lv_obj_center(row->mark_shape);
        lv_obj_set_style_radius(row->mark_shape, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(row->mark_shape, lv_color_hex(0x5C9773), 0);
        lv_obj_set_style_bg_color(row->mark_wrap, lv_color_hex(0xEEF6F0), 0);
        break;
    }

    // 轻微借用主题色参与边界收敛，避免不同页面视觉割裂。
    lv_obj_set_style_outline_width(row->mark_wrap, 1, 0);
    lv_obj_set_style_outline_pad(row->mark_wrap, 0, 0);
    lv_obj_set_style_outline_color(row->mark_wrap, lv_color_mix(theme->line, lv_color_white(), LV_OPA_40), 0);
}

static void hide_row(door_row_view_t *row)
{
    if (!row || !row->card) {
        return;
    }
    lv_obj_add_flag(row->card, LV_OBJ_FLAG_HIDDEN);
}

static void show_row(door_row_view_t *row)
{
    if (!row || !row->card) {
        return;
    }
    lv_obj_clear_flag(row->card, LV_OBJ_FLAG_HIDDEN);
}

static void update_row(door_row_view_t *row, const fridge_ui_food_t *food)
{
    char title_buf[72] = {0};
    char meta_buf[96] = {0};
    char expire_buf[24] = {0};

    if (!row || !food) {
        return;
    }

    snprintf(title_buf, sizeof(title_buf), "%s · %s",
             food->name[0] ? food->name : "待确认食材",
             food->quantity[0] ? food->quantity : "待确认");
    snprintf(meta_buf, sizeof(meta_buf), "%s · %s",
             food->location[0] ? food->location : "位置待确认",
             format_days_left(food, expire_buf, sizeof(expire_buf)));

    fridge_ui_label_set_text_if_changed(row->title, title_buf);
    fridge_ui_label_set_text_if_changed(row->meta, meta_buf);
    apply_mark_style(row, pick_mark_type(food));
    show_row(row);
}

static void create_row(lv_obj_t *parent, uint8_t index)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    door_row_view_t *row = &s_rows[index];

    row->card = lv_button_create(parent);
    lv_obj_set_size(row->card, 640, 86);
    lv_obj_set_pos(row->card, 40, 118 + index * 102);
    lv_obj_set_style_bg_color(row->card, theme->surface, 0);
    lv_obj_set_style_bg_opa(row->card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row->card, 1, 0);
    lv_obj_set_style_border_color(row->card, theme->line, 0);
    lv_obj_set_style_radius(row->card, 22, 0);
    lv_obj_set_style_shadow_width(row->card, 18, 0);
    lv_obj_set_style_shadow_opa(row->card, LV_OPA_10, 0);
    lv_obj_set_style_shadow_color(row->card, lv_color_hex(0xAB9A7A), 0);
    lv_obj_set_style_shadow_offset_y(row->card, 8, 0);
    lv_obj_set_style_pad_all(row->card, 0, 0);
    lv_obj_add_event_cb(row->card, row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    row->mark_wrap = lv_obj_create(row->card);
    lv_obj_remove_style_all(row->mark_wrap);
    lv_obj_set_size(row->mark_wrap, 48, 48);
    lv_obj_align(row->mark_wrap, LV_ALIGN_LEFT_MID, 14, 0);

    row->mark_shape = lv_obj_create(row->mark_wrap);
    lv_obj_remove_style_all(row->mark_shape);

    row->title = lv_label_create(row->card);
    lv_obj_set_width(row->title, 360);
    lv_obj_set_style_text_color(row->title, theme->text, 0);
    lv_obj_set_style_text_font(row->title, fridge_ui_font_body(), 0);
    lv_obj_align(row->title, LV_ALIGN_LEFT_MID, 76, -12);

    row->meta = lv_label_create(row->card);
    lv_obj_set_width(row->meta, 380);
    lv_obj_set_style_text_color(row->meta, theme->muted, 0);
    lv_obj_set_style_text_font(row->meta, fridge_ui_font_small(), 0);
    lv_obj_align(row->meta, LV_ALIGN_LEFT_MID, 76, 18);

    row->edit_btn = lv_button_create(row->card);
    lv_obj_set_size(row->edit_btn, 72, 42);
    lv_obj_align(row->edit_btn, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(row->edit_btn, lv_color_hex(0xFFF0D0), 0);
    lv_obj_set_style_bg_opa(row->edit_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row->edit_btn, 0, 0);
    lv_obj_set_style_shadow_width(row->edit_btn, 0, 0);
    lv_obj_set_style_radius(row->edit_btn, 18, 0);
    lv_obj_add_flag(row->edit_btn, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *edit_label = lv_label_create(row->edit_btn);
    lv_label_set_text(edit_label, "编辑");
    lv_obj_set_style_text_color(edit_label, lv_color_hex(0x8A5D00), 0);
    lv_obj_set_style_text_font(edit_label, fridge_ui_font_body(), 0);
    lv_obj_center(edit_label);
}

void fridge_ui_page_door_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();

    s_kicker = lv_label_create(parent);
    lv_label_set_text(s_kicker, "门已打开 · 本地快路径提醒");
    lv_obj_set_style_text_color(s_kicker, theme->accent, 0);
    lv_obj_set_style_text_font(s_kicker, fridge_ui_font_small(), 0);
    lv_obj_align(s_kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    s_title = lv_label_create(parent);
    lv_label_set_text(s_title, "今天优先吃什么");
    lv_obj_set_style_text_color(s_title, theme->text, 0);
    lv_obj_set_style_text_font(s_title, fridge_ui_font_title(), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 36, 38);

    for (uint8_t i = 0; i < DOOR_MAX_ROWS; i++) {
        create_row(parent, i);
    }

    s_empty_card = lv_obj_create(parent);
    lv_obj_set_pos(s_empty_card, 40, 118);
    lv_obj_set_size(s_empty_card, 640, 120);
    lv_obj_set_style_bg_color(s_empty_card, theme->surface, 0);
    lv_obj_set_style_bg_opa(s_empty_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_empty_card, 1, 0);
    lv_obj_set_style_border_color(s_empty_card, theme->line, 0);
    lv_obj_set_style_radius(s_empty_card, 22, 0);
    lv_obj_set_style_shadow_width(s_empty_card, 14, 0);
    lv_obj_set_style_shadow_opa(s_empty_card, LV_OPA_10, 0);
    lv_obj_set_style_shadow_color(s_empty_card, lv_color_hex(0xAB9A7A), 0);
    lv_obj_set_style_shadow_offset_y(s_empty_card, 6, 0);
    lv_obj_add_flag(s_empty_card, LV_OBJ_FLAG_HIDDEN);

    s_empty_title = lv_label_create(s_empty_card);
    lv_label_set_text(s_empty_title, "当前没有 3 天内临期的食材");
    lv_obj_set_style_text_color(s_empty_title, theme->text, 0);
    lv_obj_set_style_text_font(s_empty_title, fridge_ui_font_body(), 0);
    lv_obj_align(s_empty_title, LV_ALIGN_TOP_LEFT, 18, 22);

    s_empty_meta = lv_label_create(s_empty_card);
    lv_label_set_text(s_empty_meta, "可以继续从首页查看库存，或拍照登记新食材。");
    lv_obj_set_width(s_empty_meta, 560);
    lv_obj_set_style_text_color(s_empty_meta, theme->muted, 0);
    lv_obj_set_style_text_font(s_empty_meta, fridge_ui_font_small(), 0);
    lv_obj_align(s_empty_meta, LV_ALIGN_TOP_LEFT, 18, 62);
}

void fridge_ui_page_door_update(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);

    uint8_t row = 0;
    for (size_t i = 0; i < model.food_count && row < DOOR_MAX_ROWS; i++) {
        const fridge_ui_food_t *food = &model.foods[i];
        if (food->days_left > 3) {
            continue;
        }
        update_row(&s_rows[row], food);
        row++;
    }

    for (; row < DOOR_MAX_ROWS; row++) {
        hide_row(&s_rows[row]);
    }

    if (s_empty_card) {
        if (row == 0) {
            lv_obj_clear_flag(s_empty_card, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_empty_card, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
