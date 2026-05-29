// 冰箱小精灵食材编辑页。
// 该页只在用户点击保存时写入本地 cache，避免频繁写 Flash；中文输入通过轻量键盘弹层补充。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t *s_title;
static lv_obj_t *s_name;
static lv_obj_t *s_quantity;
static lv_obj_t *s_expire;
static lv_obj_t *s_location;
static lv_obj_t *s_note;
static lv_obj_t *s_place_button_label;
static char s_note_text[96];
static fridge_ui_food_t s_editing;

static void set_label_value(lv_obj_t *label, const char *name, const char *value)
{
    fridge_ui_label_set_text_fmt_if_changed(label, "%s\n%s", name, value && value[0] ? value : "未填写");
}

static void set_location_value(void)
{
    fridge_ui_label_set_text_fmt_if_changed(s_location,
                                            "位置\n%s\n位置编辑",
                                            s_editing.location[0] ? s_editing.location : "未选择");
}

static void edit_name_done(const char *text)
{
    strlcpy(s_editing.name, text ? text : "", sizeof(s_editing.name));
    set_label_value(s_name, "食材名称", s_editing.name);
}

static void edit_quantity_done(const char *text)
{
    strlcpy(s_editing.quantity, text ? text : "", sizeof(s_editing.quantity));
    set_label_value(s_quantity, "数量", s_editing.quantity);
}

static void edit_expire_done(const char *text)
{
    strlcpy(s_editing.expire, text ? text : "", sizeof(s_editing.expire));
    s_editing.days_left = (uint8_t)(atoi(s_editing.expire) > 0 ? atoi(s_editing.expire) : s_editing.days_left);
    set_label_value(s_expire, "到期时间", s_editing.expire);
}

static void edit_note_done(const char *text)
{
    strlcpy(s_note_text, text ? text : "", sizeof(s_note_text));
    fridge_ui_label_set_text_fmt_if_changed(s_note,
                                            "备注\n%s",
                                            s_note_text[0] ? s_note_text : "未输入补充信息");
}

static void field_cb(lv_event_t *event)
{
    uintptr_t field = (uintptr_t)lv_event_get_user_data(event);
    if (field == 0) {
        fridge_ui_keyboard_open_text("食材名称", s_editing.name, edit_name_done);
    } else if (field == 1) {
        fridge_ui_keyboard_open_text("数量", s_editing.quantity, edit_quantity_done);
    } else if (field == 2) {
        fridge_ui_keyboard_open_text("到期", s_editing.expire, edit_expire_done);
    } else if (field == 99) {
        fridge_ui_model_set_editing_draft(&s_editing);
        fridge_ui_model_begin_place_pick();
        fridge_ui_toast("请选择冰箱区域");
        fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
    } else {
        fridge_ui_keyboard_open_text("备注", "", edit_note_done);
    }
}

static void save_cb(lv_event_t *event)
{
    (void)event;
    if (s_editing.name[0] == '\0') {
        fridge_ui_toast("请先填写食材名称");
        return;
    }
    fridge_ui_model_update_editing_food(&s_editing);
    fridge_ui_toast("已保存");
    fridge_ui_show_page(FRIDGE_UI_PAGE_ZONE);
}

static void delete_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_model_delete_editing_food();
    fridge_ui_toast("已清空该格");
    fridge_ui_show_page(FRIDGE_UI_PAGE_ZONE);
}

static lv_obj_t *make_field(lv_obj_t *parent, int x, int y, const char *title, uintptr_t field)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, field == 99 ? 330 : 285, 82);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, theme->surface, 0);
    lv_obj_set_style_border_color(btn, theme->line, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, field_cb, LV_EVENT_CLICKED, (void *)field);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return label;
}

void fridge_ui_page_edit_food_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "食材信息编辑");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    s_title = lv_label_create(parent);
    lv_obj_set_style_text_font(s_title, fridge_ui_font_title(), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 36, 36);

    s_name = make_field(parent, 42, 98, "食材名称", 0);
    s_quantity = make_field(parent, 390, 98, "数量", 1);
    s_expire = make_field(parent, 42, 202, "到期时间", 2);
    s_location = make_field(parent, 348, 202, "位置", 99);
    s_place_button_label = s_location;
    s_note = make_field(parent, 42, 306, "语音/手动补充", 3);

    lv_obj_t *save = lv_button_create(parent);
    lv_obj_set_size(save, 210, 62);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, -42, -18);
    lv_obj_set_style_bg_color(save, theme->accent, 0);
    lv_obj_set_style_radius(save, 8, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, "保存修改");
    lv_obj_center(save_label);

    lv_obj_t *del = lv_button_create(parent);
    lv_obj_set_size(del, 160, 62);
    lv_obj_align(del, LV_ALIGN_BOTTOM_LEFT, 42, -18);
    lv_obj_set_style_bg_color(del, theme->danger, 0);
    lv_obj_set_style_radius(del, 8, 0);
    lv_obj_set_style_shadow_width(del, 0, 0);
    lv_obj_add_event_cb(del, delete_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del_label = lv_label_create(del);
    lv_label_set_text(del_label, "清空格子");
    lv_obj_center(del_label);
}

void fridge_ui_page_edit_food_update(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    s_editing = model.editing_food;
    fridge_ui_label_set_text_fmt_if_changed(s_title, "修改记录 · %s", model.zones[s_editing.zone].name);
    set_label_value(s_name, "食材名称", s_editing.name);
    set_label_value(s_quantity, "数量", s_editing.quantity);
    set_label_value(s_expire, "到期时间", s_editing.expire);
    set_location_value();
    if (s_place_button_label) {
        set_location_value();
    }
    fridge_ui_label_set_text_fmt_if_changed(s_note,
                                            "备注\n%s",
                                            s_note_text[0] ? s_note_text : "点击输入补充信息");
}
