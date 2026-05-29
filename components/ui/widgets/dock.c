// 冰箱小精灵 UI 底部 Dock。
// 第一阶段只开放待机、首页、设置和 Wi-Fi；AI/相机/更多先保留灰色占位，避免触发未完成业务链路。

#include "fridge_ui_internal.h"

typedef struct {
    const char *text;
    const char *icon;
    fridge_ui_page_t page;
    bool enabled;
    bool primary;
} dock_item_t;

static lv_obj_t *s_buttons[5];
static lv_obj_t *s_icons[5];
static lv_obj_t *s_labels[5];
static lv_obj_t *s_indicators[5];
static bool s_last_active[5];

static void dock_btn_cb(lv_event_t *event)
{
    dock_item_t *item = lv_event_get_user_data(event);
    if (!item || !item->enabled) {
        fridge_ui_toast("暂未实现");
        return;
    }
    fridge_ui_show_page(item->page);
}

lv_obj_t *fridge_ui_dock_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    static dock_item_t items[] = {
        {"首页", LV_SYMBOL_HOME, FRIDGE_UI_PAGE_HOME, true, false},
        {"AI", "+", FRIDGE_UI_PAGE_AI, true, false},
        {"登记", LV_SYMBOL_IMAGE, FRIDGE_UI_PAGE_CAMERA, true, true},
        {"设置", LV_SYMBOL_SETTINGS, FRIDGE_UI_PAGE_SETTINGS, true, false},
        {"更多", LV_SYMBOL_BARS, FRIDGE_UI_PAGE_MORE, true, false},
    };

    lv_obj_t *dock = lv_obj_create(parent);
    lv_obj_remove_style_all(dock);
    lv_obj_set_pos(dock, 32, 632);
    lv_obj_set_size(dock, FRIDGE_DISPLAY_WIDTH - 64, 72);
    lv_obj_set_style_bg_color(dock, theme->surface, 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_set_style_border_color(dock, theme->line, 0);
    lv_obj_set_style_radius(dock, 30, 0);
    lv_obj_set_style_pad_all(dock, 8, 0);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(dock, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    for (size_t i = 0; i < 5; i++) {
        lv_obj_t *btn = lv_button_create(dock);
        lv_obj_set_size(btn, items[i].primary ? 118 : 126, items[i].primary ? 88 : 56);
        lv_obj_set_style_radius(btn, items[i].primary ? 30 : 22, 0);
        lv_obj_set_style_bg_color(btn, items[i].primary ? theme->accent_2 : theme->surface, 0);
        lv_obj_set_style_bg_opa(btn, items[i].primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(btn, items[i].primary ? 18 : 0, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0xD95745), 0);
        lv_obj_set_style_translate_y(btn, items[i].primary ? -20 : 0, 0);
        lv_obj_set_style_pad_top(btn, items[i].primary ? 8 : 4, 0);
        lv_obj_add_event_cb(btn, dock_btn_cb, LV_EVENT_CLICKED, &items[i]);

        // 非主按钮不再用整块绿色背景表示选中，避免切页大面积重绘时盖住底部内容。
        lv_obj_t *indicator = lv_obj_create(btn);
        lv_obj_remove_style_all(indicator);
        lv_obj_set_size(indicator, 34, 4);
        lv_obj_set_style_bg_color(indicator, theme->accent, 0);
        lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(indicator, 2, 0);
        lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, 2);
        lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, items[i].icon);
        lv_obj_set_style_text_color(icon, items[i].primary ? lv_color_white() : theme->muted, 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, items[i].text);
        lv_obj_set_style_text_color(label, items[i].primary ? lv_color_white() : theme->muted, 0);
        lv_obj_set_style_text_font(label, fridge_ui_font_small(), 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -2);
        s_buttons[i] = btn;
        s_icons[i] = icon;
        s_labels[i] = label;
        s_indicators[i] = indicator;
    }
    return dock;
}

void fridge_ui_dock_update(void)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    for (size_t i = 0; i < 5; i++) {
        if (!s_buttons[i]) {
            continue;
        }
        bool active = (i == 0 && g_ui_page == FRIDGE_UI_PAGE_STANDBY) ||
                      (i == 0 && (g_ui_page == FRIDGE_UI_PAGE_HOME || g_ui_page == FRIDGE_UI_PAGE_ZONE || g_ui_page == FRIDGE_UI_PAGE_EDIT_FOOD || g_ui_page == FRIDGE_UI_PAGE_DOOR)) ||
                      (i == 1 && (g_ui_page == FRIDGE_UI_PAGE_AI || g_ui_page == FRIDGE_UI_PAGE_RECIPE)) ||
                      (i == 2 && (g_ui_page == FRIDGE_UI_PAGE_CAMERA || g_ui_page == FRIDGE_UI_PAGE_CAMERA_RESULT)) ||
                      (i == 3 && (g_ui_page == FRIDGE_UI_PAGE_SETTINGS || g_ui_page == FRIDGE_UI_PAGE_WIFI)) ||
                      (i == 4 && (g_ui_page == FRIDGE_UI_PAGE_MORE || g_ui_page == FRIDGE_UI_PAGE_SHOPPING || g_ui_page == FRIDGE_UI_PAGE_OFFLINE || g_ui_page == FRIDGE_UI_PAGE_NUTRITION || g_ui_page == FRIDGE_UI_PAGE_TIMER || g_ui_page == FRIDGE_UI_PAGE_STOPWATCH || g_ui_page == FRIDGE_UI_PAGE_ALARM));
        if (s_last_active[i] == active) {
            continue;
        }
        s_last_active[i] = active;
        bool primary = (i == 2);
        lv_color_t text_color = active ? theme->accent : theme->muted;
        lv_obj_set_style_bg_color(s_buttons[i], primary ? theme->accent_2 : theme->surface, 0);
        lv_obj_set_style_bg_opa(s_buttons[i], primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (s_icons[i]) {
            lv_obj_set_style_text_color(s_icons[i], primary ? lv_color_white() : text_color, 0);
        }
        if (s_labels[i]) {
            lv_obj_set_style_text_color(s_labels[i], primary ? lv_color_white() : text_color, 0);
        }
        if (s_indicators[i]) {
            if (active && !primary) {
                lv_obj_remove_flag(s_indicators[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_indicators[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}
