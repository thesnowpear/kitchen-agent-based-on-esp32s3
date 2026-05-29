// 冰箱小精灵待机页。
// 待机时保持黑底和超大线条颜文字；这里只绘制 LVGL 对象，不调整背光、电源、触摸或显示总线。

#include "fridge_ui_internal.h"

#include "esp_timer.h"

static lv_obj_t *s_left_eye;
static lv_obj_t *s_right_eye;
static lv_obj_t *s_mouth;
static lv_obj_t *s_mouth_group;
static uint8_t s_face_index;
static int64_t s_last_face_ms;

static const lv_point_precise_t EYE_CARET_POINTS[] = {
    {0, 76},
    {72, 0},
    {144, 76},
};

static const lv_point_precise_t EYE_FLAT_POINTS[] = {
    {0, 0},
    {160, 0},
};

static const lv_point_precise_t EYE_ROUND_POINTS[] = {
    {60, 0},
    {103, 18},
    {120, 60},
    {103, 103},
    {60, 120},
    {18, 103},
    {0, 60},
    {18, 18},
    {60, 0},
};

static const lv_point_precise_t MOUTH_CARET_POINTS[] = {
    {0, 76},
    {72, 0},
    {144, 76},
};

static const lv_point_precise_t MOUTH_FLAT_POINTS[] = {
    {0, 0},
    {190, 0},
};

static const lv_point_precise_t MOUTH_ROUND_POINTS[] = {
    {48, 0},
    {82, 14},
    {96, 48},
    {82, 82},
    {48, 96},
    {14, 82},
    {0, 48},
    {14, 14},
    {48, 0},
};

typedef enum {
    STANDBY_FACE_SMILE = 0,
    STANDBY_FACE_SLEEPY,
    STANDBY_FACE_CURIOUS,
    STANDBY_FACE_NEUTRAL,
    STANDBY_FACE_COUNT,
} standby_face_t;

static lv_obj_t *create_line(lv_obj_t *parent, const lv_point_precise_t *points, uint32_t point_count)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, points, point_count);
    lv_obj_set_style_line_color(line, lv_color_white(), 0);
    lv_obj_set_style_line_width(line, 20, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

static void place_eye(lv_obj_t *eye, lv_coord_t x, lv_coord_t y)
{
    lv_obj_set_pos(eye, x, y);
}

static void set_line_points(lv_obj_t *line, const lv_point_precise_t *points, uint32_t point_count)
{
    lv_line_set_points(line, points, point_count);
}

static void apply_face(standby_face_t face)
{
    switch (face) {
    case STANDBY_FACE_SMILE:
        set_line_points(s_left_eye, EYE_CARET_POINTS, sizeof(EYE_CARET_POINTS) / sizeof(EYE_CARET_POINTS[0]));
        set_line_points(s_right_eye, EYE_CARET_POINTS, sizeof(EYE_CARET_POINTS) / sizeof(EYE_CARET_POINTS[0]));
        set_line_points(s_mouth, MOUTH_CARET_POINTS, sizeof(MOUTH_CARET_POINTS) / sizeof(MOUTH_CARET_POINTS[0]));
        place_eye(s_left_eye, 160, 230);
        place_eye(s_right_eye, 416, 230);
        lv_obj_set_pos(s_mouth_group, 288, 374);
        break;
    case STANDBY_FACE_SLEEPY:
        set_line_points(s_left_eye, EYE_FLAT_POINTS, sizeof(EYE_FLAT_POINTS) / sizeof(EYE_FLAT_POINTS[0]));
        set_line_points(s_right_eye, EYE_FLAT_POINTS, sizeof(EYE_FLAT_POINTS) / sizeof(EYE_FLAT_POINTS[0]));
        set_line_points(s_mouth, MOUTH_FLAT_POINTS, sizeof(MOUTH_FLAT_POINTS) / sizeof(MOUTH_FLAT_POINTS[0]));
        place_eye(s_left_eye, 150, 282);
        place_eye(s_right_eye, 410, 282);
        lv_obj_set_pos(s_mouth_group, 265, 414);
        break;
    case STANDBY_FACE_CURIOUS:
        set_line_points(s_left_eye, EYE_ROUND_POINTS, sizeof(EYE_ROUND_POINTS) / sizeof(EYE_ROUND_POINTS[0]));
        set_line_points(s_right_eye, EYE_ROUND_POINTS, sizeof(EYE_ROUND_POINTS) / sizeof(EYE_ROUND_POINTS[0]));
        set_line_points(s_mouth, MOUTH_ROUND_POINTS, sizeof(MOUTH_ROUND_POINTS) / sizeof(MOUTH_ROUND_POINTS[0]));
        place_eye(s_left_eye, 172, 226);
        place_eye(s_right_eye, 428, 226);
        lv_obj_set_pos(s_mouth_group, 312, 398);
        break;
    case STANDBY_FACE_NEUTRAL:
    default:
        set_line_points(s_left_eye, EYE_FLAT_POINTS, sizeof(EYE_FLAT_POINTS) / sizeof(EYE_FLAT_POINTS[0]));
        set_line_points(s_right_eye, EYE_FLAT_POINTS, sizeof(EYE_FLAT_POINTS) / sizeof(EYE_FLAT_POINTS[0]));
        set_line_points(s_mouth, MOUTH_FLAT_POINTS, sizeof(MOUTH_FLAT_POINTS) / sizeof(MOUTH_FLAT_POINTS[0]));
        place_eye(s_left_eye, 150, 250);
        place_eye(s_right_eye, 410, 250);
        lv_obj_set_pos(s_mouth_group, 265, 410);
        break;
    }
}

static void wake_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
}

void fridge_ui_page_standby_create(lv_obj_t *parent)
{
    lv_obj_add_event_cb(parent, wake_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);

    s_left_eye = create_line(parent, EYE_CARET_POINTS, sizeof(EYE_CARET_POINTS) / sizeof(EYE_CARET_POINTS[0]));
    s_right_eye = create_line(parent, EYE_CARET_POINTS, sizeof(EYE_CARET_POINTS) / sizeof(EYE_CARET_POINTS[0]));

    s_mouth_group = lv_obj_create(parent);
    lv_obj_remove_style_all(s_mouth_group);
    lv_obj_set_size(s_mouth_group, 220, 110);
    lv_obj_remove_flag(s_mouth_group, LV_OBJ_FLAG_SCROLLABLE);
    s_mouth = create_line(s_mouth_group, MOUTH_CARET_POINTS, sizeof(MOUTH_CARET_POINTS) / sizeof(MOUTH_CARET_POINTS[0]));

    apply_face(STANDBY_FACE_SMILE);
}

void fridge_ui_page_standby_update(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_face_ms > 2600) {
        s_face_index = (s_face_index + 1) % STANDBY_FACE_COUNT;
        apply_face((standby_face_t)s_face_index);
        s_last_face_ms = now_ms;
    }
}
