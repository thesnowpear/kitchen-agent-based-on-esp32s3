// 冰箱小精灵拍照结果确认页。
// 只展示 OV3660 + AI 生成的候选，用户确认后才写入本地库存。

#include "fridge_ui_internal.h"

static lv_obj_t *s_result;
static lv_obj_t *s_meta;
static lv_obj_t *s_note;
static lv_obj_t *s_confirm;
static lv_obj_t *s_edit;

static fridge_ui_camera_result_t current_result(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    return model.camera_result;
}

static void confirm_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_camera_result_t result = current_result();
    if (!result.valid || !result.ok) {
        fridge_ui_toast("还没有可确认的识别结果");
        return;
    }
    fridge_ui_model_add_camera_food(result.name, result.quantity, result.zone, result.cell);
    fridge_ui_toast("已登记，继续拍摄");
    fridge_ui_show_page(FRIDGE_UI_PAGE_CAMERA);
}

static void edit_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_camera_result_t result = current_result();
    if (!result.valid || !result.ok) {
        fridge_ui_toast("请先重新拍照识别");
        fridge_ui_show_page(FRIDGE_UI_PAGE_CAMERA);
        return;
    }
    fridge_ui_model_add_camera_food(result.name, result.quantity, result.zone, result.cell);
    fridge_ui_model_select_cell(result.zone, result.cell);
    fridge_ui_show_page(FRIDGE_UI_PAGE_EDIT_FOOD);
}

void fridge_ui_page_camera_result_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "拍照登记 · AI 识别结果");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "确认食材信息");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 640, 286);
    lv_obj_set_pos(card, 40, 92);
    lv_obj_set_style_bg_color(card, theme->surface, 0);
    lv_obj_set_style_border_color(card, theme->line, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 14, 0);

    s_result = lv_label_create(card);
    lv_obj_set_style_text_color(s_result, theme->accent, 0);
    lv_obj_set_style_text_font(s_result, fridge_ui_font_title(), 0);
    lv_obj_set_width(s_result, 580);
    lv_obj_align(s_result, LV_ALIGN_TOP_LEFT, 22, 20);

    s_meta = lv_label_create(card);
    lv_obj_set_style_text_color(s_meta, theme->text, 0);
    lv_obj_set_style_text_font(s_meta, fridge_ui_font_body(), 0);
    lv_obj_set_width(s_meta, 580);
    lv_obj_align(s_meta, LV_ALIGN_TOP_LEFT, 22, 86);

    s_note = lv_label_create(parent);
    lv_label_set_text(s_note, "AI 结果只作为候选，确认后才会写入库存");
    lv_obj_set_style_text_color(s_note, theme->muted, 0);
    lv_obj_set_style_text_font(s_note, fridge_ui_font_small(), 0);
    lv_obj_align(s_note, LV_ALIGN_TOP_LEFT, 54, 396);

    s_confirm = lv_button_create(parent);
    lv_obj_set_size(s_confirm, 260, 64);
    lv_obj_align(s_confirm, LV_ALIGN_BOTTOM_RIGHT, -48, -18);
    lv_obj_set_style_bg_color(s_confirm, theme->accent, 0);
    lv_obj_set_style_bg_color(s_confirm, theme->surface_soft, LV_STATE_DISABLED);
    lv_obj_set_style_radius(s_confirm, 8, 0);
    lv_obj_add_event_cb(s_confirm, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *confirm_label = lv_label_create(s_confirm);
    lv_label_set_text(confirm_label, "确认放入");
    lv_obj_center(confirm_label);

    s_edit = lv_button_create(parent);
    lv_obj_set_size(s_edit, 190, 64);
    lv_obj_align(s_edit, LV_ALIGN_BOTTOM_LEFT, 48, -18);
    lv_obj_set_style_bg_color(s_edit, theme->surface_soft, 0);
    lv_obj_set_style_radius(s_edit, 8, 0);
    lv_obj_add_event_cb(s_edit, edit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *edit_label = lv_label_create(s_edit);
    lv_label_set_text(edit_label, "修改");
    lv_obj_center(edit_label);
}

void fridge_ui_page_camera_result_update(void)
{
    fridge_ui_camera_result_t result = current_result();
    bool can_confirm = result.valid && result.ok;
    if (!result.valid) {
        fridge_ui_label_set_text_if_changed(s_result, "还没有识别结果");
        fridge_ui_label_set_text_if_changed(s_meta, "请返回拍照登记页，对准食材后点击快门。");
    } else if (result.analyzing) {
        fridge_ui_label_set_text_if_changed(s_result, "正在识别...");
        fridge_ui_label_set_text_if_changed(s_meta, result.meta[0] ? result.meta : "正在上传照片给 AI。");
    } else if (!result.ok) {
        fridge_ui_label_set_text_if_changed(s_result, "识别失败");
        fridge_ui_label_set_text_if_changed(s_meta, result.error[0] ? result.error : "请检查网络、API Key 或摄像头接线后重试。");
    } else {
        fridge_ui_label_set_text_fmt_if_changed(s_result, "%s · %s", result.name, result.quantity);
        fridge_ui_label_set_text_fmt_if_changed(s_meta,
                                                "%s\n图像 %dx%d · JPEG %u 字节 · AI %lu ms",
                                                result.meta[0] ? result.meta : "请确认食材和数量。",
                                                result.width,
                                                result.height,
                                                (unsigned)result.jpeg_bytes,
                                                (unsigned long)result.latency_ms);
    }

    if (can_confirm) {
        lv_obj_remove_state(s_confirm, LV_STATE_DISABLED);
        lv_obj_remove_state(s_edit, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_confirm, LV_STATE_DISABLED);
        lv_obj_add_state(s_edit, LV_STATE_DISABLED);
    }
}
