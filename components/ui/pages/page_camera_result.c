// 冰箱小精灵拍照结果确认页。
// 用户确认后写入本地库存；后续可把候选来源替换为 OV3660 + AI 识别结果。

#include "fridge_ui_internal.h"

static lv_obj_t *s_result;
static lv_obj_t *s_meta;
static lv_obj_t *s_note;

static uint8_t s_sample_index;

typedef struct {
    const char *name;
    const char *quantity;
    uint8_t zone;
    uint8_t cell;
    const char *meta;
} camera_sample_t;

static const camera_sample_t SAMPLES[] = {
    {"番茄", "2 个", 1, 4, "建议放入左侧冷藏 B2\n外皮完整，适合 3 天内食用"},
    {"鸡蛋", "6 个", 3, 0, "建议放入门架 A1\n生产日期已记录，优先一周内使用"},
    {"菠菜", "1 把", 2, 3, "建议放入右侧冷藏 B1\n叶片新鲜，适合煮汤"},
};

static const camera_sample_t *current_sample(void)
{
    return &SAMPLES[s_sample_index % (sizeof(SAMPLES) / sizeof(SAMPLES[0]))];
}

static void confirm_cb(lv_event_t *event)
{
    (void)event;
    const camera_sample_t *sample = current_sample();
    fridge_ui_model_add_camera_food(sample->name, sample->quantity, sample->zone, sample->cell);
    fridge_ui_toast("已登记，继续拍摄");
    s_sample_index++;
    fridge_ui_show_page(FRIDGE_UI_PAGE_CAMERA);
}

static void edit_cb(lv_event_t *event)
{
    (void)event;
    const camera_sample_t *sample = current_sample();
    fridge_ui_model_add_camera_food(sample->name, sample->quantity, sample->zone, sample->cell);
    fridge_ui_model_select_cell(sample->zone, sample->cell);
    fridge_ui_show_page(FRIDGE_UI_PAGE_EDIT_FOOD);
}

void fridge_ui_page_camera_result_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "拍照登记 · 识别结果");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "确认食材信息");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 640, 250);
    lv_obj_set_pos(card, 40, 100);
    lv_obj_set_style_bg_color(card, theme->surface, 0);
    lv_obj_set_style_border_color(card, theme->line, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);

    s_result = lv_label_create(card);
    lv_obj_set_style_text_color(s_result, theme->accent, 0);
    lv_obj_set_style_text_font(s_result, fridge_ui_font_title(), 0);
    lv_obj_align(s_result, LV_ALIGN_TOP_LEFT, 22, 20);

    s_meta = lv_label_create(card);
    lv_obj_set_style_text_color(s_meta, theme->text, 0);
    lv_obj_set_style_text_font(s_meta, fridge_ui_font_body(), 0);
    lv_obj_set_width(s_meta, 580);
    lv_obj_align(s_meta, LV_ALIGN_TOP_LEFT, 22, 86);

    s_note = lv_label_create(parent);
    lv_label_set_text(s_note, "语音补充登记信息  ·  手动补充登记信息");
    lv_obj_set_style_text_color(s_note, theme->muted, 0);
    lv_obj_set_style_text_font(s_note, fridge_ui_font_small(), 0);
    lv_obj_align(s_note, LV_ALIGN_TOP_LEFT, 54, 372);

    lv_obj_t *confirm = lv_button_create(parent);
    lv_obj_set_size(confirm, 260, 64);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, -48, -18);
    lv_obj_set_style_bg_color(confirm, theme->accent, 0);
    lv_obj_set_style_radius(confirm, 8, 0);
    lv_obj_add_event_cb(confirm, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *confirm_label = lv_label_create(confirm);
    lv_label_set_text(confirm_label, "确认放入");
    lv_obj_center(confirm_label);

    lv_obj_t *edit = lv_button_create(parent);
    lv_obj_set_size(edit, 190, 64);
    lv_obj_align(edit, LV_ALIGN_BOTTOM_LEFT, 48, -18);
    lv_obj_set_style_bg_color(edit, theme->surface_soft, 0);
    lv_obj_set_style_radius(edit, 8, 0);
    lv_obj_add_event_cb(edit, edit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *edit_label = lv_label_create(edit);
    lv_label_set_text(edit_label, "修改");
    lv_obj_center(edit_label);
}

void fridge_ui_page_camera_result_update(void)
{
    const camera_sample_t *sample = current_sample();
    fridge_ui_label_set_text_fmt_if_changed(s_result, "%s · %s", sample->name, sample->quantity);
    fridge_ui_label_set_text_if_changed(s_meta, sample->meta);
}
