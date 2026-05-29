// 冰箱小精灵营养助手页。
// 本页只根据本地库存名称做轻量膳食结构提示，不联网、不引入营养数据库，避免阻塞 LVGL 任务。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t total;
    uint8_t expiring;
    uint8_t protein;
    uint8_t vegetable;
    uint8_t fruit;
    uint8_t dairy;
    uint8_t staple;
} nutrition_summary_t;

static lv_obj_t *s_score;
static lv_obj_t *s_score_meta;
static lv_obj_t *s_expiring;
static lv_obj_t *s_balance;
static lv_obj_t *s_protein_fill;
static lv_obj_t *s_veg_fill;
static lv_obj_t *s_dairy_fill;
static lv_obj_t *s_protein_label;
static lv_obj_t *s_veg_label;
static lv_obj_t *s_dairy_label;
static lv_obj_t *s_tip_title;
static lv_obj_t *s_tip_body;
static lv_obj_t *s_restrict_body;

static const lv_font_t *nutrition_font_small(void)
{
    // 营养页小字包含“膳食、忌口、过敏源”等开放文案，复用 AI 对话页更完整的 16px 中文字库。
    return fridge_ui_font_ai_body();
}

static bool name_has_any(const char *name, const char *const *keys, size_t key_count)
{
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < key_count; i++) {
        if (keys[i] && strstr(name, keys[i])) {
            return true;
        }
    }
    return false;
}

static bool is_protein_food(const char *name)
{
    static const char *const keys[] = {"蛋", "鸡", "牛", "鱼", "虾", "肉", "豆腐", "豆", "火腿"};
    return name_has_any(name, keys, sizeof(keys) / sizeof(keys[0]));
}

static bool is_vegetable_food(const char *name)
{
    static const char *const keys[] = {"菜", "番茄", "西红柿", "黄瓜", "菠菜", "生菜", "胡萝卜", "土豆", "蘑菇", "菌"};
    return name_has_any(name, keys, sizeof(keys) / sizeof(keys[0]));
}

static bool is_fruit_food(const char *name)
{
    static const char *const keys[] = {"苹果", "香蕉", "橙", "梨", "莓", "葡萄", "西瓜", "芒果", "水果"};
    return name_has_any(name, keys, sizeof(keys) / sizeof(keys[0]));
}

static bool is_dairy_food(const char *name)
{
    static const char *const keys[] = {"奶", "酸奶", "牛乳", "芝士", "奶酪"};
    return name_has_any(name, keys, sizeof(keys) / sizeof(keys[0]));
}

static bool is_staple_food(const char *name)
{
    static const char *const keys[] = {"米", "饭", "面", "馒头", "面包", "饺", "粉", "玉米", "燕麦"};
    return name_has_any(name, keys, sizeof(keys) / sizeof(keys[0]));
}

static void recipe_btn_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_RECIPE);
}

static void shopping_btn_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_SHOPPING);
}

static void restrict_btn_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_toast("忌口偏好后续接入手机端同步");
}

static lv_obj_t *create_card(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, int16_t h, lv_color_t bg)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, bg, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, theme->line, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_shadow_width(card, 12, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_10, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x9A8A70), 0);
    lv_obj_set_style_shadow_offset_y(card, 6, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *create_metric_card(lv_obj_t *parent, const char *label, int16_t x, lv_color_t tint)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *card = create_card(parent, x, 114, 204, 116, lv_color_mix(tint, theme->surface, LV_OPA_10));

    lv_obj_t *cap = lv_label_create(card);
    lv_label_set_text(cap, label);
    lv_obj_set_style_text_color(cap, theme->muted, 0);
    lv_obj_set_style_text_font(cap, nutrition_font_small(), 0);
    lv_obj_set_pos(cap, 18, 16);

    lv_obj_t *value = lv_label_create(card);
    lv_obj_set_style_text_color(value, theme->text, 0);
    lv_obj_set_style_text_font(value, fridge_ui_font_large(), 0);
    lv_obj_set_pos(value, 18, 50);
    return value;
}

static void create_bar(lv_obj_t *parent, const char *name, int16_t y, lv_color_t tint, lv_obj_t **fill, lv_obj_t **label)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, name);
    lv_obj_set_style_text_color(title, theme->text, 0);
    lv_obj_set_style_text_font(title, nutrition_font_small(), 0);
    lv_obj_set_pos(title, 22, y);

    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_pos(track, 118, y + 4);
    lv_obj_set_size(track, 230, 12);
    lv_obj_set_style_bg_color(track, lv_color_mix(theme->line, theme->surface, LV_OPA_60), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 6, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    *fill = lv_obj_create(track);
    lv_obj_remove_style_all(*fill);
    lv_obj_set_pos(*fill, 0, 0);
    lv_obj_set_size(*fill, 0, 12);
    lv_obj_set_style_bg_color(*fill, tint, 0);
    lv_obj_set_style_bg_opa(*fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(*fill, 6, 0);
    lv_obj_remove_flag(*fill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    *label = lv_label_create(parent);
    lv_obj_set_style_text_color(*label, theme->muted, 0);
    lv_obj_set_style_text_font(*label, nutrition_font_small(), 0);
    lv_obj_set_pos(*label, 364, y - 2);
}

static lv_obj_t *create_action_btn(lv_obj_t *parent, const char *text, int16_t x, lv_event_cb_t cb, lv_color_t tint)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 196, 52);
    lv_obj_set_pos(btn, x, 522);
    lv_obj_set_style_bg_color(btn, tint, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_body(), 0);
    lv_obj_center(label);
    return btn;
}

static nutrition_summary_t build_summary(const fridge_ui_model_t *model)
{
    nutrition_summary_t summary = {0};
    if (!model) {
        return summary;
    }

    summary.total = (uint8_t)(model->food_count > 255 ? 255 : model->food_count);
    for (size_t i = 0; i < model->food_count && i < FRIDGE_UI_MAX_FOODS; i++) {
        const fridge_ui_food_t *food = &model->foods[i];
        if (food->name[0] == '\0') {
            continue;
        }
        if (food->days_left <= 3) {
            summary.expiring++;
        }
        if (is_protein_food(food->name)) {
            summary.protein++;
        }
        if (is_vegetable_food(food->name)) {
            summary.vegetable++;
        }
        if (is_fruit_food(food->name)) {
            summary.fruit++;
        }
        if (is_dairy_food(food->name)) {
            summary.dairy++;
        }
        if (is_staple_food(food->name)) {
            summary.staple++;
        }
    }
    return summary;
}

static uint8_t compute_score(const nutrition_summary_t *summary)
{
    uint8_t score = 42;
    if (!summary || summary->total == 0) {
        return 0;
    }
    if (summary->vegetable + summary->fruit >= 2) {
        score += 22;
    } else if (summary->vegetable + summary->fruit >= 1) {
        score += 12;
    }
    if (summary->protein >= 1) {
        score += 16;
    }
    if (summary->dairy >= 1) {
        score += 10;
    }
    if (summary->staple >= 1) {
        score += 6;
    }
    if (summary->expiring >= 3) {
        score -= 8;
    }
    return score > 99 ? 99 : score;
}

static void update_bar(lv_obj_t *fill, lv_obj_t *label, uint8_t value, uint8_t target)
{
    const uint8_t capped = value > target ? target : value;
    int16_t width = (int16_t)((230 * capped) / target);
    if (width < 18 && value > 0) {
        width = 18;
    }
    lv_obj_set_width(fill, width);
    fridge_ui_label_set_text_fmt_if_changed(label, "%u/%u", (unsigned)value, (unsigned)target);
}

static void update_recommendation(const nutrition_summary_t *summary)
{
    if (!summary || summary->total == 0) {
        fridge_ui_label_set_text_if_changed(s_tip_title, "先登记几样食材");
        fridge_ui_label_set_text_if_changed(s_tip_body, "拍照或手动登记后，我会根据库存给出更贴近当前冰箱的营养建议。");
        return;
    }

    if (summary->protein == 0) {
        fridge_ui_label_set_text_if_changed(s_tip_title, "今日建议补一点蛋白");
        fridge_ui_label_set_text_if_changed(s_tip_body, "库存里暂未看到蛋、奶、豆制品或肉鱼虾；晚餐可以搭配鸡蛋、豆腐或鱼类。");
    } else if (summary->vegetable + summary->fruit == 0) {
        fridge_ui_label_set_text_if_changed(s_tip_title, "蔬果库存偏少");
        fridge_ui_label_set_text_if_changed(s_tip_body, "当前库存更偏主食/蛋白，建议下次采购补充叶菜、番茄或水果。");
    } else if (summary->expiring > 0) {
        fridge_ui_label_set_text_if_changed(s_tip_title, "优先消耗临期食材");
        fridge_ui_label_set_text_if_changed(s_tip_body, "先把 3 天内到期的食材安排进今日菜单，减少浪费，也能让推荐更贴近实际。");
    } else {
        fridge_ui_label_set_text_if_changed(s_tip_title, "结构看起来不错");
        fridge_ui_label_set_text_if_changed(s_tip_body, "库存里已有蔬果和蛋白来源，可以让 AI 菜谱按“清淡、少油、快手”生成搭配。");
    }
}

void fridge_ui_page_nutrition_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_bottom(parent, 100, 0);

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "本地库存营养观察");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, nutrition_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "营养助手");
    lv_obj_set_style_text_color(title, theme->text, 0);
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, "按库存粗略判断膳食结构，适合做日常提醒，不替代专业营养建议。");
    lv_obj_set_width(hint, 640);
    lv_obj_set_style_text_color(hint, theme->muted, 0);
    lv_obj_set_style_text_font(hint, nutrition_font_small(), 0);
    lv_obj_set_pos(hint, 38, 78);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);

    s_score = create_metric_card(parent, "均衡分", 30, lv_color_hex(0x6B8F71));
    s_expiring = create_metric_card(parent, "临期食材", 258, lv_color_hex(0xD95745));
    s_balance = create_metric_card(parent, "膳食结构", 486, lv_color_hex(0xE0A941));

    lv_obj_t *score_card = lv_obj_get_parent(s_score);
    s_score_meta = lv_label_create(score_card);
    lv_obj_set_style_text_color(s_score_meta, theme->muted, 0);
    lv_obj_set_style_text_font(s_score_meta, nutrition_font_small(), 0);
    lv_obj_set_pos(s_score_meta, 18, 86);

    lv_obj_t *balance_card = create_card(parent, 30, 250, 430, 158, theme->surface);
    lv_obj_t *balance_title = lv_label_create(balance_card);
    lv_label_set_text(balance_title, "库存结构");
    lv_obj_set_style_text_color(balance_title, theme->text, 0);
    lv_obj_set_style_text_font(balance_title, fridge_ui_font_body(), 0);
    lv_obj_set_pos(balance_title, 22, 16);
    create_bar(balance_card, "蛋白", 52, lv_color_hex(0xD95745), &s_protein_fill, &s_protein_label);
    create_bar(balance_card, "蔬果", 84, lv_color_hex(0x6B8F71), &s_veg_fill, &s_veg_label);
    create_bar(balance_card, "乳品", 116, lv_color_hex(0x4E82A6), &s_dairy_fill, &s_dairy_label);

    lv_obj_t *tip = create_card(parent, 484, 250, 206, 158, lv_color_hex(0xFFF7E6));
    s_tip_title = lv_label_create(tip);
    lv_obj_set_width(s_tip_title, 166);
    lv_obj_set_style_text_color(s_tip_title, theme->text, 0);
    lv_obj_set_style_text_font(s_tip_title, nutrition_font_small(), 0);
    lv_obj_set_pos(s_tip_title, 18, 18);
    lv_label_set_long_mode(s_tip_title, LV_LABEL_LONG_DOT);

    s_tip_body = lv_label_create(tip);
    lv_obj_set_width(s_tip_body, 166);
    lv_obj_set_style_text_color(s_tip_body, theme->muted, 0);
    lv_obj_set_style_text_font(s_tip_body, nutrition_font_small(), 0);
    lv_obj_set_pos(s_tip_body, 18, 58);
    lv_label_set_long_mode(s_tip_body, LV_LABEL_LONG_WRAP);

    lv_obj_t *restrict_card = create_card(parent, 30, 424, 660, 82, lv_color_hex(0xF4F9F2));
    lv_obj_t *restrict_title = lv_label_create(restrict_card);
    lv_label_set_text(restrict_title, "忌口提醒");
    lv_obj_set_style_text_color(restrict_title, theme->text, 0);
    lv_obj_set_style_text_font(restrict_title, fridge_ui_font_body(), 0);
    lv_obj_set_pos(restrict_title, 20, 14);

    s_restrict_body = lv_label_create(restrict_card);
    lv_obj_set_width(s_restrict_body, 440);
    lv_obj_set_style_text_color(s_restrict_body, theme->muted, 0);
    lv_obj_set_style_text_font(s_restrict_body, nutrition_font_small(), 0);
    lv_obj_set_pos(s_restrict_body, 20, 46);
    lv_label_set_long_mode(s_restrict_body, LV_LABEL_LONG_DOT);

    create_action_btn(parent, "生成菜谱", 42, recipe_btn_cb, theme->accent_2);
    create_action_btn(parent, "补齐采购", 262, shopping_btn_cb, lv_color_hex(0x438C74));
    create_action_btn(parent, "设置忌口", 482, restrict_btn_cb, lv_color_hex(0x8C7A43));
}

void fridge_ui_page_nutrition_update(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    nutrition_summary_t summary = build_summary(&model);
    uint8_t score = compute_score(&summary);
    uint8_t veg_fruit = summary.vegetable + summary.fruit;
    uint8_t groups = 0;
    groups += summary.protein > 0 ? 1 : 0;
    groups += veg_fruit > 0 ? 1 : 0;
    groups += summary.dairy > 0 ? 1 : 0;
    groups += summary.staple > 0 ? 1 : 0;

    fridge_ui_label_set_text_fmt_if_changed(s_score, "%u", (unsigned)score);
    fridge_ui_label_set_text_if_changed(s_score_meta, score >= 80 ? "搭配较完整" : (score >= 60 ? "还可补齐" : "需要登记/补货"));
    fridge_ui_label_set_text_fmt_if_changed(s_expiring, "%u 项", (unsigned)summary.expiring);
    fridge_ui_label_set_text_fmt_if_changed(s_balance, "%u/4 类", (unsigned)groups);
    update_bar(s_protein_fill, s_protein_label, summary.protein, 2);
    update_bar(s_veg_fill, s_veg_label, veg_fruit, 3);
    update_bar(s_dairy_fill, s_dairy_label, summary.dairy, 1);
    update_recommendation(&summary);
    fridge_ui_label_set_text_if_changed(s_restrict_body, "当前仅做本地提示；低糖、低盐、过敏源等偏好后续由手机端同步。");
}
