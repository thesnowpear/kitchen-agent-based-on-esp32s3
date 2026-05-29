// 冰箱小精灵首页总览。
// 对齐 ui-reference 新版首页：顶部状态文案、冰箱二维分区图，以及首页内联“编辑空间”面板。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t zone;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    lv_color_t bg;
    lv_obj_t *button;
    lv_obj_t *name;
    lv_obj_t *count;
} home_zone_view_t;

static home_zone_view_t s_zone_views[4];
static home_zone_view_t s_custom_views[2];
static lv_obj_t *s_map_outer;
static lv_obj_t *s_map;
static lv_obj_t *s_add_zone_btn;
static lv_obj_t *s_edit_space_label;
static lv_obj_t *s_expiring_label;
static lv_obj_t *s_editor_panel;
static lv_obj_t *s_editor_title;
static lv_obj_t *s_name_value;
static lv_obj_t *s_note_value;
static lv_obj_t *s_width_value;
static lv_obj_t *s_height_value;
static lv_obj_t *s_delete_btn;
static lv_obj_t *s_scroll_hint;
static bool s_space_editing;
static uint8_t s_selected_zone = UINT8_MAX;

static uint8_t clamp_span(uint8_t value)
{
    if (value < 1) {
        return 1;
    }
    if (value > 3) {
        return 3;
    }
    return value;
}

static uint8_t count_expiring_items(const fridge_ui_model_t *model)
{
    uint8_t count = 0;
    if (!model) {
        return 0;
    }
    for (size_t i = 0; i < model->food_count; i++) {
        if (model->foods[i].days_left <= 2) {
            count++;
        }
    }
    return count;
}

static bool is_custom_zone_visible(const fridge_ui_model_t *model, uint8_t zone)
{
    if (!model || zone >= FRIDGE_UI_ZONE_COUNT) {
        return false;
    }
    const char *name = model->zones[zone].name;
    const bool placeholder = strcmp(name, "自定义区") == 0 || strcmp(name, "备用区") == 0;
    return model->zones[zone].custom && (model->zones[zone].count > 0 || (name[0] != '\0' && !placeholder));
}

static bool has_free_custom_slot(const bool custom_visible[2])
{
    return !custom_visible[0] || !custom_visible[1];
}

static void select_zone_for_edit(uint8_t zone)
{
    if (zone >= FRIDGE_UI_ZONE_COUNT) {
        return;
    }
    s_selected_zone = zone;
    fridge_ui_model_set_active_zone(zone);
    fridge_ui_page_home_update();
}

static void update_zone_from_editor(const char *name, const char *note, uint8_t width, uint8_t height)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    if (s_selected_zone >= FRIDGE_UI_ZONE_COUNT) {
        return;
    }
    const fridge_ui_zone_summary_t *zone = &model.zones[s_selected_zone];
    fridge_ui_model_update_zone(s_selected_zone,
                                name && name[0] ? name : zone->name,
                                width ? width : zone->width,
                                height ? height : zone->height,
                                note ? note : zone->note);
    fridge_ui_page_home_update();
}

static void name_done_cb(const char *text)
{
    update_zone_from_editor(text, NULL, 0, 0);
}

static void note_done_cb(const char *text)
{
    update_zone_from_editor(NULL, text, 0, 0);
}

static void zone_cb(lv_event_t *event)
{
    uintptr_t zone = (uintptr_t)lv_event_get_user_data(event);
    if (fridge_ui_model_is_place_picking()) {
        fridge_ui_model_set_active_zone((uint8_t)zone);
        fridge_ui_show_page(FRIDGE_UI_PAGE_ZONE);
        return;
    }
    if (s_space_editing) {
        select_zone_for_edit((uint8_t)zone);
        return;
    }
    fridge_ui_model_set_active_zone((uint8_t)zone);
    fridge_ui_show_page(FRIDGE_UI_PAGE_ZONE);
}

static void expiring_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_DOOR);
}

static void edit_space_cb(lv_event_t *event)
{
    (void)event;
    s_space_editing = !s_space_editing;
    if (s_space_editing && s_selected_zone >= FRIDGE_UI_ZONE_COUNT) {
        s_selected_zone = 0;
    }
    fridge_ui_page_home_update();
}

static void add_zone_cb(lv_event_t *event)
{
    (void)event;
    if (fridge_ui_model_add_custom_zone()) {
        fridge_ui_model_t model = {0};
        fridge_ui_model_get(&model);
        s_selected_zone = model.active_zone;
        fridge_ui_toast("已添加自定义区域");
    } else {
        fridge_ui_toast("最多两个自定义区域");
    }
    s_space_editing = true;
    fridge_ui_page_home_update();
}

static void name_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    if (s_selected_zone < FRIDGE_UI_ZONE_COUNT) {
        fridge_ui_keyboard_open_text("区域名称", model.zones[s_selected_zone].name, name_done_cb);
    }
}

static void note_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    if (s_selected_zone < FRIDGE_UI_ZONE_COUNT) {
        fridge_ui_keyboard_open_text("备注", model.zones[s_selected_zone].note, note_done_cb);
    }
}

static void width_cb(lv_event_t *event)
{
    intptr_t delta = (intptr_t)lv_event_get_user_data(event);
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    if (s_selected_zone >= FRIDGE_UI_ZONE_COUNT) {
        return;
    }
    int width_next = (int)model.zones[s_selected_zone].width + (int)delta;
    uint8_t width = clamp_span((uint8_t)(width_next < 1 ? 1 : width_next));
    update_zone_from_editor(NULL, NULL, width, 0);
}

static void height_cb(lv_event_t *event)
{
    intptr_t delta = (intptr_t)lv_event_get_user_data(event);
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    if (s_selected_zone >= FRIDGE_UI_ZONE_COUNT) {
        return;
    }
    int height_next = (int)model.zones[s_selected_zone].height + (int)delta;
    uint8_t height = clamp_span((uint8_t)(height_next < 1 ? 1 : height_next));
    update_zone_from_editor(NULL, NULL, 0, height);
}

static void delete_cb(lv_event_t *event)
{
    (void)event;
    if (s_selected_zone >= 4 && s_selected_zone < FRIDGE_UI_ZONE_COUNT) {
        fridge_ui_model_delete_zone(s_selected_zone);
        s_selected_zone = 0;
        fridge_ui_toast("已删除当前区域");
    } else {
        fridge_ui_toast("标准分区暂不删除");
    }
    fridge_ui_page_home_update();
}

static lv_obj_t *create_action_button(lv_obj_t *parent, const char *text, int16_t x, int16_t w, lv_event_cb_t cb)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, 26);
    lv_obj_set_size(btn, w, 50);
    lv_obj_set_style_bg_color(btn, theme->accent, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_pad_left(btn, 10, 0);
    lv_obj_set_style_pad_right(btn, 10, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_body(), 0);
    lv_obj_center(label);
    return label;
}

static void apply_zone_layout(home_zone_view_t *view, int16_t x, int16_t y, int16_t w, int16_t h)
{
    if (!view || !view->button) {
        return;
    }
    view->x = x;
    view->y = y;
    view->w = w;
    view->h = h;
    lv_obj_set_pos(view->button, x, y);
    lv_obj_set_size(view->button, w, h);
    lv_obj_set_width(view->name, w > 28 ? w - 28 : w);
    lv_obj_set_width(view->count, w > 28 ? w - 28 : w);
    lv_label_set_long_mode(view->name, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(view->count, LV_LABEL_LONG_DOT);
    lv_obj_align(view->name, LV_ALIGN_CENTER, 0, h > 120 ? -20 : -14);
    lv_obj_align(view->count, LV_ALIGN_CENTER, 0, h > 120 ? 22 : 18);
}

static void set_edit_target_style(home_zone_view_t *view, bool selected)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    if (!view || !view->button) {
        return;
    }
    lv_obj_set_style_border_width(view->button, selected ? 4 : 1, 0);
    lv_obj_set_style_border_color(view->button, selected ? theme->accent_2 : theme->line, 0);
}

static void create_zone_button(home_zone_view_t *view, lv_obj_t *map)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *btn = lv_button_create(map);
    lv_obj_set_pos(btn, view->x, view->y);
    lv_obj_set_size(btn, view->w, view->h);
    lv_obj_set_style_bg_color(btn, view->bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, theme->line, 0);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, zone_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)view->zone);
    view->button = btn;

    view->name = lv_label_create(btn);
    lv_obj_set_style_text_color(view->name, theme->muted, 0);
    lv_obj_set_style_text_font(view->name, fridge_ui_font_body(), 0);
    lv_obj_set_style_text_align(view->name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(view->name, LV_ALIGN_CENTER, 0, -18);

    view->count = lv_label_create(btn);
    lv_obj_set_style_text_color(view->count, theme->text, 0);
    lv_obj_set_style_text_font(view->count, fridge_ui_font_large(), 0);
    lv_obj_set_style_text_align(view->count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(view->count, LV_ALIGN_CENTER, 0, 22);
}

static lv_obj_t *create_add_zone_button(lv_obj_t *map)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *btn = lv_button_create(map);
    lv_obj_set_style_bg_color(btn, theme->surface_soft, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, theme->accent, 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, add_zone_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, "添加区域\n+");
    lv_obj_set_style_text_color(label, theme->accent, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_body(), 0);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *create_editor_button(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, const char *title, lv_event_cb_t cb)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, 32);
    lv_obj_set_style_bg_color(btn, theme->surface, 0);
    lv_obj_set_style_border_color(btn, theme->line, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, theme->accent, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_small(), 0);
    lv_obj_center(label);
    return label;
}

static lv_obj_t *create_step_button(lv_obj_t *parent, int16_t x, int16_t y, const char *text, lv_event_cb_t cb, intptr_t delta)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, 32, 32);
    lv_obj_set_style_bg_color(btn, theme->surface_soft, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)delta);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, theme->accent, 0);
    lv_obj_center(label);
    return btn;
}

static void create_editor_panel(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    s_editor_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_editor_panel);
    lv_obj_set_pos(s_editor_panel, 28, 558);
    lv_obj_set_size(s_editor_panel, 664, 184);
    lv_obj_set_style_bg_color(s_editor_panel, theme->surface, 0);
    lv_obj_set_style_bg_opa(s_editor_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_editor_panel, theme->line, 0);
    lv_obj_set_style_border_width(s_editor_panel, 1, 0);
    lv_obj_set_style_radius(s_editor_panel, 20, 0);
    lv_obj_set_style_pad_all(s_editor_panel, 8, 0);
    lv_obj_add_flag(s_editor_panel, LV_OBJ_FLAG_HIDDEN);

    s_editor_title = lv_label_create(s_editor_panel);
    lv_obj_set_pos(s_editor_title, 14, 10);
    lv_obj_set_style_text_color(s_editor_title, theme->accent, 0);
    lv_obj_set_style_text_font(s_editor_title, fridge_ui_font_body(), 0);

    s_name_value = create_editor_button(s_editor_panel, 14, 54, 300, "名称", name_cb);
    lv_obj_set_width(s_name_value, 276);
    lv_label_set_long_mode(s_name_value, LV_LABEL_LONG_DOT);
    s_note_value = create_editor_button(s_editor_panel, 332, 54, 300, "备注", note_cb);
    lv_obj_set_width(s_note_value, 276);
    lv_label_set_long_mode(s_note_value, LV_LABEL_LONG_DOT);

    lv_obj_t *width_title = lv_label_create(s_editor_panel);
    lv_label_set_text(width_title, "宽度");
    lv_obj_set_pos(width_title, 24, 106);
    lv_obj_set_style_text_color(width_title, theme->muted, 0);
    lv_obj_set_style_text_font(width_title, fridge_ui_font_small(), 0);
    create_step_button(s_editor_panel, 96, 102, "-", width_cb, -1);
    s_width_value = lv_label_create(s_editor_panel);
    lv_obj_set_pos(s_width_value, 142, 108);
    lv_obj_set_style_text_color(s_width_value, theme->text, 0);
    lv_obj_set_style_text_font(s_width_value, fridge_ui_font_body(), 0);
    create_step_button(s_editor_panel, 188, 102, "+", width_cb, 1);

    lv_obj_t *height_title = lv_label_create(s_editor_panel);
    lv_label_set_text(height_title, "高度");
    lv_obj_set_pos(height_title, 260, 106);
    lv_obj_set_style_text_color(height_title, theme->muted, 0);
    lv_obj_set_style_text_font(height_title, fridge_ui_font_small(), 0);
    create_step_button(s_editor_panel, 332, 102, "-", height_cb, -1);
    s_height_value = lv_label_create(s_editor_panel);
    lv_obj_set_pos(s_height_value, 378, 108);
    lv_obj_set_style_text_color(s_height_value, theme->text, 0);
    lv_obj_set_style_text_font(s_height_value, fridge_ui_font_body(), 0);
    create_step_button(s_editor_panel, 424, 102, "+", height_cb, 1);

    s_delete_btn = lv_button_create(s_editor_panel);
    lv_obj_set_pos(s_delete_btn, 516, 104);
    lv_obj_set_size(s_delete_btn, 116, 44);
    lv_obj_set_style_bg_color(s_delete_btn, theme->danger, 0);
    lv_obj_set_style_shadow_width(s_delete_btn, 0, 0);
    lv_obj_set_style_radius(s_delete_btn, 12, 0);
    lv_obj_add_event_cb(s_delete_btn, delete_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del_label = lv_label_create(s_delete_btn);
    lv_label_set_text(del_label, "删除");
    lv_obj_set_style_text_color(del_label, lv_color_white(), 0);
    lv_obj_center(del_label);
}

void fridge_ui_page_home_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);
    // 屏幕刷新率有限，关闭惯性/弹性滚动，避免松手后继续刷新造成明显拖影。
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_bottom(parent, 110, 0);

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "检测到有人靠近 · 已启动");
    lv_obj_set_style_text_color(kicker, lv_color_hex(0x5F8F72), 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_body(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 30, 4);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "冰箱总览");
    lv_obj_set_style_text_color(title, theme->text, 0);
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 30, 38);

    s_edit_space_label = create_action_button(parent, "编辑空间", 394, 130, edit_space_cb);
    s_expiring_label = create_action_button(parent, "临期 0", 540, 140, expiring_cb);

    s_map_outer = lv_obj_create(parent);
    lv_obj_remove_style_all(s_map_outer);
    lv_obj_set_pos(s_map_outer, 28, 82);
    lv_obj_set_size(s_map_outer, 664, 426);
    lv_obj_set_style_bg_color(s_map_outer, theme->surface_panel, 0);
    lv_obj_set_style_bg_opa(s_map_outer, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_map_outer, 36, 0);
    lv_obj_set_style_pad_all(s_map_outer, 10, 0);
    lv_obj_set_style_border_width(s_map_outer, 7, 0);
    lv_obj_set_style_border_color(s_map_outer, lv_color_hex(0xE6ECE3), 0);

    s_map = lv_obj_create(s_map_outer);
    lv_obj_remove_style_all(s_map);
    lv_obj_set_pos(s_map, 14, 14);
    lv_obj_set_size(s_map, 626, 378);
    lv_obj_set_style_bg_color(s_map, lv_color_hex(0xF7FAF4), 0);
    lv_obj_set_style_bg_opa(s_map, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_map, 28, 0);
    lv_obj_set_style_border_width(s_map, 4, 0);
    lv_obj_set_style_border_color(s_map, lv_color_hex(0xD8E1D5), 0);

    s_zone_views[0] = (home_zone_view_t){.zone = 0, .bg = lv_color_hex(0xF1F8FA)};
    s_zone_views[1] = (home_zone_view_t){.zone = 1, .bg = lv_color_hex(0xFFFDF2)};
    s_zone_views[2] = (home_zone_view_t){.zone = 2, .bg = lv_color_hex(0xF6FCF3)};
    s_zone_views[3] = (home_zone_view_t){.zone = 3, .bg = lv_color_hex(0xFFF7EA)};
    for (size_t i = 0; i < 4; i++) {
        create_zone_button(&s_zone_views[i], s_map);
    }

    s_custom_views[0] = (home_zone_view_t){.zone = 4, .bg = lv_color_hex(0xF3F7FF)};
    s_custom_views[1] = (home_zone_view_t){.zone = 5, .bg = lv_color_hex(0xF4F2FF)};
    create_zone_button(&s_custom_views[0], s_map);
    create_zone_button(&s_custom_views[1], s_map);
    s_add_zone_btn = create_add_zone_button(s_map);

    s_scroll_hint = lv_label_create(parent);
    lv_label_set_text(s_scroll_hint, "向下滑动编辑区域");
    lv_obj_set_pos(s_scroll_hint, 36, 524);
    lv_obj_set_style_text_color(s_scroll_hint, theme->muted, 0);
    lv_obj_set_style_text_font(s_scroll_hint, fridge_ui_font_small(), 0);
    lv_obj_add_flag(s_scroll_hint, LV_OBJ_FLAG_HIDDEN);

    create_editor_panel(parent);
}

static void update_editor_panel(const fridge_ui_model_t *model)
{
    if (!s_editor_panel || !model) {
        return;
    }
    if (!s_space_editing) {
        lv_obj_add_flag(s_editor_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s_selected_zone >= FRIDGE_UI_ZONE_COUNT) {
        s_selected_zone = 0;
    }
    lv_obj_remove_flag(s_editor_panel, LV_OBJ_FLAG_HIDDEN);
    const fridge_ui_zone_summary_t *zone = &model->zones[s_selected_zone];
    fridge_ui_label_set_text_fmt_if_changed(s_editor_title, "编辑：%s", zone->name);
    fridge_ui_label_set_text_fmt_if_changed(s_name_value, "名称 %s", zone->name);
    fridge_ui_label_set_text_fmt_if_changed(s_note_value, "备注 %s", zone->note[0] ? zone->note : "可放食材");
    fridge_ui_label_set_text_fmt_if_changed(s_width_value, "%u", clamp_span(zone->width));
    fridge_ui_label_set_text_fmt_if_changed(s_height_value, "%u", clamp_span(zone->height));
    if (s_delete_btn) {
        if (s_selected_zone >= 4) {
            lv_obj_clear_state(s_delete_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_delete_btn, LV_STATE_DISABLED);
        }
    }
}

void fridge_ui_page_home_update(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    bool picking = fridge_ui_model_is_place_picking();

    if (s_expiring_label) {
        fridge_ui_label_set_text_fmt_if_changed(s_expiring_label, "临期 %u", count_expiring_items(&model));
    }
    if (s_edit_space_label) {
        fridge_ui_label_set_text_if_changed(s_edit_space_label, s_space_editing ? "完成编辑" : "编辑空间");
    }

    bool custom_visible[2] = {
        is_custom_zone_visible(&model, 4),
        is_custom_zone_visible(&model, 5),
    };
    bool edit_or_custom = s_space_editing || picking || custom_visible[0] || custom_visible[1];
    bool free_custom_slot = has_free_custom_slot(custom_visible);

    apply_zone_layout(&s_zone_views[0], 18, 18, 452, 96);
    apply_zone_layout(&s_zone_views[1], 18, 128, 218, 188);
    apply_zone_layout(&s_zone_views[2], 250, 128, 220, 188);
    apply_zone_layout(&s_zone_views[3], 484, 18, 124, 298);

    for (size_t i = 0; i < 4; i++) {
        home_zone_view_t *view = &s_zone_views[i];
        fridge_ui_label_set_text_if_changed(view->name, model.zones[view->zone].name);
        fridge_ui_label_set_text_fmt_if_changed(view->count, "%u 件", model.zones[view->zone].count);
        set_edit_target_style(view, (s_space_editing && s_selected_zone == view->zone) || (picking && model.active_zone == view->zone));
    }

    int16_t next_x = 18;
    int16_t next_y = 334;
    int16_t row_h = 0;
    int16_t content_bottom = 316;
    const int16_t grid_gap = 12;
    const int16_t cell_w = 194;
    const int16_t cell_h = 96;
    for (size_t i = 0; i < 2; i++) {
        home_zone_view_t *view = &s_custom_views[i];
        bool visible = custom_visible[i];
        if (!visible) {
            lv_obj_add_flag(view->button, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(view->button, LV_OBJ_FLAG_HIDDEN);
        uint8_t width = clamp_span(model.zones[view->zone].width);
        uint8_t height = clamp_span(model.zones[view->zone].height);
        int16_t w = (int16_t)(width * cell_w + (width - 1) * grid_gap);
        int16_t h = (int16_t)(height * cell_h + (height - 1) * grid_gap);
        if (next_x > 18 && next_x + w > 608) {
            next_x = 18;
            next_y += row_h + grid_gap;
            row_h = 0;
        }
        apply_zone_layout(view, next_x, next_y, w, h);
        fridge_ui_label_set_text_if_changed(view->name, model.zones[view->zone].name);
        const char *note = model.zones[view->zone].note[0] ? model.zones[view->zone].note : "可放食材";
        if (model.zones[view->zone].count > 0 && !s_space_editing) {
            fridge_ui_label_set_text_fmt_if_changed(view->count, "%u 件", model.zones[view->zone].count);
            lv_obj_set_style_text_font(view->count, fridge_ui_font_large(), 0);
        } else {
            fridge_ui_label_set_text_if_changed(view->count, note);
            lv_obj_set_style_text_font(view->count, fridge_ui_font_body(), 0);
        }
        set_edit_target_style(view, (s_space_editing && s_selected_zone == view->zone) || (picking && model.active_zone == view->zone));
        next_x += w + grid_gap;
        if (h > row_h) {
            row_h = h;
        }
        if (next_y + h > content_bottom) {
            content_bottom = next_y + h;
        }
    }

    if (s_add_zone_btn) {
        if (s_space_editing && free_custom_slot) {
            lv_obj_remove_flag(s_add_zone_btn, LV_OBJ_FLAG_HIDDEN);
            int16_t add_w = cell_w;
            int16_t add_h = cell_h;
            if (next_x > 18 && next_x + add_w > 608) {
                next_x = 18;
                next_y += row_h + grid_gap;
                row_h = 0;
            }
            lv_obj_set_pos(s_add_zone_btn, next_x, next_y);
            lv_obj_set_size(s_add_zone_btn, add_w, add_h);
            if (next_y + add_h > content_bottom) {
                content_bottom = next_y + add_h;
            }
        } else {
            lv_obj_add_flag(s_add_zone_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    int16_t map_h = edit_or_custom ? (content_bottom + 18) : 378;
    if (map_h < (s_space_editing ? 408 : 378)) {
        map_h = s_space_editing ? 408 : 378;
    }
    int16_t outer_h = map_h + 48;
    if (s_map_outer && s_map) {
        lv_obj_set_pos(s_map_outer, 28, 82);
        lv_obj_set_size(s_map_outer, 664, outer_h);
        lv_obj_set_size(s_map, 626, map_h);
    }

    if (s_scroll_hint) {
        if (s_space_editing) {
            lv_obj_remove_flag(s_scroll_hint, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_scroll_hint, 36, 82 + outer_h + 16);
        } else {
            lv_obj_add_flag(s_scroll_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_editor_panel) {
        lv_obj_set_pos(s_editor_panel, 28, s_space_editing ? (82 + outer_h + 48) : 444);
    }

    update_editor_panel(&model);
}
