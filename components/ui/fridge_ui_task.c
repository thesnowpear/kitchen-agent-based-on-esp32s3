// 冰箱小精灵 LVGL UI 主任务。
// 负责初始化 TR230S 显示、FT6336U 触摸、LVGL tick/flush/read 回调，并创建五个核心页面。

#include "fridge_ui_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_ai_actions.h"
#include "fridge_display.h"
#include "fridge_state_machine.h"
#include "fridge_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UI_TASK_STACK 24576
#define UI_TASK_PRIORITY 5
#define UI_BUFFER_ROWS 120
#define UI_MODEL_POLL_MS 1000
#define UI_TICK_MS 5
#define UI_TOAST_MS 1800
#define UI_IDLE_SLEEP_MS 120000
#define UI_TOUCH_WAKE_GRACE_MS UI_IDLE_SLEEP_MS

static const char *TAG = "fridge_ui";

lv_obj_t *g_ui_root;
fridge_ui_page_t g_ui_page = FRIDGE_UI_PAGE_HOME;

static TaskHandle_t s_ui_task;
static lv_display_t *s_display;
static lv_obj_t *s_pages[FRIDGE_UI_PAGE_COUNT];
static lv_obj_t *s_content;
static lv_obj_t *s_status_bar;
static lv_obj_t *s_dock;
static lv_obj_t *s_toast;
static int64_t s_toast_until_ms;
static int64_t s_last_activity_ms;
static int64_t s_last_radar_seen_ms;
static int64_t s_last_model_poll_ms;
static int64_t s_touch_wake_until_ms;
static fridge_sm_state_t s_last_auto_state = FRIDGE_SM_STATE_SLEEP;
static bool s_auto_standby_active;

typedef struct {
    fridge_ui_page_t page;
    char toast[96];
} ui_page_async_request_t;

static void ui_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(UI_TICK_MS);
}

static void ui_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_err_t ret = fridge_display_flush_area((uint16_t)area->x1,
                                              (uint16_t)area->y1,
                                              (uint16_t)area->x2,
                                              (uint16_t)area->y2,
                                              (const uint16_t *)px_map);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "display flush failed: %s", esp_err_to_name(ret));
    }
    lv_display_flush_ready(disp);
}

static void ui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    fridge_touch_point_t point = {0};
    if (fridge_touch_read(&point) == ESP_OK && point.pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = point.x;
        data->point.y = point.y;
        fridge_ui_note_activity();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static lv_obj_t *make_page(void)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *page = lv_obj_create(s_content);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, theme->bg, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

static void update_current_page(void)
{
    bool standby = g_ui_page == FRIDGE_UI_PAGE_STANDBY;
    if (s_status_bar) {
        if (standby) {
            lv_obj_add_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_dock) {
        if (standby) {
            lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_content) {
        lv_obj_set_pos(s_content, 0, standby ? 0 : 82);
        lv_obj_set_size(s_content, FRIDGE_DISPLAY_WIDTH, standby ? FRIDGE_DISPLAY_HEIGHT : 538);
    }
    fridge_ui_status_bar_update();
    fridge_ui_dock_update();
    switch (g_ui_page) {
    case FRIDGE_UI_PAGE_STANDBY:
        fridge_ui_page_standby_update();
        break;
    case FRIDGE_UI_PAGE_HOME:
        fridge_ui_page_home_update();
        break;
    case FRIDGE_UI_PAGE_ZONE:
        fridge_ui_page_zone_update();
        break;
    case FRIDGE_UI_PAGE_EDIT_FOOD:
        fridge_ui_page_edit_food_update();
        break;
    case FRIDGE_UI_PAGE_DOOR:
        fridge_ui_page_door_update();
        break;
    case FRIDGE_UI_PAGE_CAMERA:
        fridge_ui_page_camera_update();
        break;
    case FRIDGE_UI_PAGE_CAMERA_RESULT:
        fridge_ui_page_camera_result_update();
        break;
    case FRIDGE_UI_PAGE_RECIPE:
        fridge_ui_page_recipe_update();
        break;
    case FRIDGE_UI_PAGE_NUTRITION:
        fridge_ui_page_nutrition_update();
        break;
    case FRIDGE_UI_PAGE_AI:
        fridge_ui_page_ai_update();
        break;
    case FRIDGE_UI_PAGE_SHOPPING:
        fridge_ui_page_shopping_update();
        break;
    case FRIDGE_UI_PAGE_SETTINGS:
        fridge_ui_page_settings_update();
        break;
    case FRIDGE_UI_PAGE_WIFI:
        fridge_ui_page_wifi_update();
        break;
    case FRIDGE_UI_PAGE_MORE:
        fridge_ui_page_more_update();
        break;
    case FRIDGE_UI_PAGE_OFFLINE:
        fridge_ui_page_offline_update();
        break;
    case FRIDGE_UI_PAGE_TIMER:
        fridge_ui_page_timer_update();
        break;
    case FRIDGE_UI_PAGE_STOPWATCH:
        fridge_ui_page_stopwatch_update();
        break;
    case FRIDGE_UI_PAGE_ALARM:
        fridge_ui_page_alarm_update();
        break;
    default:
        break;
    }
}

static void apply_state_machine_ui_policy(void)
{
    fridge_sm_snapshot_t sm = {0};
    fridge_sm_config_t config = {0};
    if (fridge_state_machine_get_snapshot(&sm) != ESP_OK) {
        return;
    }
    (void)fridge_state_machine_get_config(&config);

    int64_t now_ms = esp_timer_get_time() / 1000;
    bool radar_seen = sm.radar_presence_reliable || sm.radar_within_2m ||
                      sm.radar_within_1m || sm.radar_approaching;
    if (radar_seen) {
        s_last_radar_seen_ms = now_ms;
    }
    if (s_last_activity_ms == 0) {
        s_last_activity_ms = now_ms;
    }
    if (s_last_radar_seen_ms == 0) {
        s_last_radar_seen_ms = now_ms;
    }

    bool user_idle = now_ms - s_last_activity_ms >= UI_IDLE_SLEEP_MS;
    bool radar_idle = now_ms - s_last_radar_seen_ms >= UI_IDLE_SLEEP_MS;
    bool touch_wake_grace = now_ms < s_touch_wake_until_ms;
    bool no_human_context = !touch_wake_grace && !radar_seen && radar_idle;
    bool quiet_state = sm.state == FRIDGE_SM_STATE_SLEEP ||
                       sm.state == FRIDGE_SM_STATE_NIGHT_SAVE ||
                       sm.state == FRIDGE_SM_STATE_OFFLINE;

    // 状态机只给 UI 线程下达页面/亮度策略；这里再叠加“2 分钟无人/无互动”的屏幕策略。
    // 休眠开关打开：必须同时满足雷达 2 分钟无人体上下文、用户 2 分钟无互动，才真正黑屏。
    // 休眠开关关闭：用户 2 分钟无互动或雷达 2 分钟无人体上下文时，只退回颜文字待机页并保持可触摸唤醒。
    if (config.sleep_enabled && quiet_state && user_idle && no_human_context) {
        if (g_ui_page != FRIDGE_UI_PAGE_STANDBY) {
            s_auto_standby_active = true;
            fridge_ui_show_page(FRIDGE_UI_PAGE_STANDBY);
            s_auto_standby_active = false;
        }
        (void)fridge_display_set_brightness(0);
    } else {
        if (!config.sleep_enabled && quiet_state && (user_idle || no_human_context) && g_ui_page != FRIDGE_UI_PAGE_STANDBY) {
            s_auto_standby_active = true;
            fridge_ui_show_page(FRIDGE_UI_PAGE_STANDBY);
            s_auto_standby_active = false;
        }
        (void)fridge_display_set_brightness(65);
    }

    if (sm.state == s_last_auto_state) {
        return;
    }
    s_last_auto_state = sm.state;

    switch (sm.state) {
    case FRIDGE_SM_STATE_SLEEP:
    case FRIDGE_SM_STATE_NIGHT_SAVE:
        // 休眠/无人状态不立即抢占页面；由上面的 2 分钟 idle 策略决定黑屏或退回颜文字页。
        break;
    case FRIDGE_SM_STATE_APPROACH:
        fridge_ui_show_page(FRIDGE_UI_PAGE_STANDBY);
        break;
    case FRIDGE_SM_STATE_INTERACTIVE:
        fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
        break;
    case FRIDGE_SM_STATE_DOOR_MOVING:
    case FRIDGE_SM_STATE_DOOR_OPEN:
    case FRIDGE_SM_STATE_POST_CLOSE:
        fridge_ui_show_page(FRIDGE_UI_PAGE_DOOR);
        break;
    case FRIDGE_SM_STATE_OFFLINE:
        // 离线是网络标志，不再抢占成本地整页；离线页保留给用户从更多页手动查看。
        break;
    default:
        break;
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    esp_err_t display_ret = ESP_FAIL;
    while ((display_ret = fridge_display_init()) != ESP_OK) {
        // 屏幕排线/WAIT#/供电异常时不要让 UI 任务返回导致整机反复重启；保留 USB 控制台继续排查。
        ESP_LOGE(TAG, "display init failed: %s; retry in 2s", esp_err_to_name(display_ret));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    esp_err_t touch_ret = fridge_touch_init();
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed, UI continues without touch: %s", esp_err_to_name(touch_ret));
    }

    lv_init();
    size_t buf_pixels = FRIDGE_DISPLAY_WIDTH * UI_BUFFER_ROWS;
    void *buf1 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *buf2 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "LVGL draw buffer allocation failed");
        vTaskDelete(NULL);
    }

    s_display = lv_display_create(FRIDGE_DISPLAY_WIDTH, FRIDGE_DISPLAY_HEIGHT);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, buf1, buf2, buf_pixels * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, ui_flush_cb);
    ESP_LOGI(TAG, "LVGL draw buffers ready, rows=%u bytes_each=%u",
             (unsigned)UI_BUFFER_ROWS,
             (unsigned)(buf_pixels * sizeof(lv_color_t)));

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, ui_touch_read_cb);

    const esp_timer_create_args_t tick_args = {
        .callback = ui_tick_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, UI_TICK_MS * 1000));

    fridge_ui_model_init();
    fridge_ui_model_poll();

    g_ui_root = lv_screen_active();
    fridge_ui_theme_apply(g_ui_root);

    s_status_bar = fridge_ui_status_bar_create(g_ui_root);
    s_content = lv_obj_create(g_ui_root);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_pos(s_content, 0, 82);
    lv_obj_set_size(s_content, FRIDGE_DISPLAY_WIDTH, 538);
    lv_obj_set_style_bg_color(s_content, fridge_ui_theme_get()->bg, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_COVER, 0);

    for (uint8_t i = 0; i < FRIDGE_UI_PAGE_COUNT; i++) {
        s_pages[i] = make_page();
    }

    fridge_ui_page_standby_create(s_pages[FRIDGE_UI_PAGE_STANDBY]);
    fridge_ui_page_home_create(s_pages[FRIDGE_UI_PAGE_HOME]);
    fridge_ui_page_zone_create(s_pages[FRIDGE_UI_PAGE_ZONE]);
    fridge_ui_page_edit_food_create(s_pages[FRIDGE_UI_PAGE_EDIT_FOOD]);
    fridge_ui_page_door_create(s_pages[FRIDGE_UI_PAGE_DOOR]);
    fridge_ui_page_camera_create(s_pages[FRIDGE_UI_PAGE_CAMERA]);
    fridge_ui_page_camera_result_create(s_pages[FRIDGE_UI_PAGE_CAMERA_RESULT]);
    fridge_ui_page_recipe_create(s_pages[FRIDGE_UI_PAGE_RECIPE]);
    fridge_ui_page_nutrition_create(s_pages[FRIDGE_UI_PAGE_NUTRITION]);
    fridge_ui_page_ai_create(s_pages[FRIDGE_UI_PAGE_AI]);
    fridge_ui_page_shopping_create(s_pages[FRIDGE_UI_PAGE_SHOPPING]);
    fridge_ui_page_settings_create(s_pages[FRIDGE_UI_PAGE_SETTINGS]);
    fridge_ui_page_wifi_create(s_pages[FRIDGE_UI_PAGE_WIFI]);
    fridge_ui_page_more_create(s_pages[FRIDGE_UI_PAGE_MORE]);
    fridge_ui_page_offline_create(s_pages[FRIDGE_UI_PAGE_OFFLINE]);
    fridge_ui_page_timer_create(s_pages[FRIDGE_UI_PAGE_TIMER]);
    fridge_ui_page_stopwatch_create(s_pages[FRIDGE_UI_PAGE_STOPWATCH]);
    fridge_ui_page_alarm_create(s_pages[FRIDGE_UI_PAGE_ALARM]);
    s_dock = fridge_ui_dock_create(g_ui_root);

    s_toast = lv_label_create(g_ui_root);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x292520), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_white(), 0);
    lv_obj_set_style_pad_all(s_toast, 14, 0);
    lv_obj_set_style_radius(s_toast, 8, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -132);

    // 重启/重新烧录后先进入首页；后续再由 2 分钟 idle 策略退回颜文字或黑屏。
    fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
    ESP_LOGI(TAG, "LVGL UI started");

    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_last_model_poll_ms >= UI_MODEL_POLL_MS) {
            fridge_ui_model_poll();
            apply_state_machine_ui_policy();
            update_current_page();
            s_last_model_poll_ms = now_ms;
        }
        if (s_toast && s_toast_until_ms > 0 && now_ms >= s_toast_until_ms) {
            lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
            s_toast_until_ms = 0;
        }
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void ui_set_page_async_cb(void *user_data)
{
    ui_page_async_request_t *request = (ui_page_async_request_t *)user_data;
    if (!request) {
        return;
    }
    if (request->page < FRIDGE_UI_PAGE_COUNT) {
        fridge_ui_show_page(request->page);
        if (request->toast[0] != '\0') {
            fridge_ui_toast(request->toast);
        }
    }
    free(request);
}

bool fridge_ui_page_from_key(const char *key, fridge_ui_page_t *out)
{
    if (!key || !out) {
        return false;
    }
    if (strcmp(key, "standby") == 0) {
        *out = FRIDGE_UI_PAGE_STANDBY;
    } else if (strcmp(key, "home") == 0) {
        *out = FRIDGE_UI_PAGE_HOME;
    } else if (strcmp(key, "zone") == 0) {
        *out = FRIDGE_UI_PAGE_ZONE;
    } else if (strcmp(key, "door") == 0) {
        *out = FRIDGE_UI_PAGE_DOOR;
    } else if (strcmp(key, "recipe") == 0) {
        *out = FRIDGE_UI_PAGE_RECIPE;
    } else if (strcmp(key, "nutrition") == 0) {
        *out = FRIDGE_UI_PAGE_NUTRITION;
    } else if (strcmp(key, "shopping") == 0) {
        *out = FRIDGE_UI_PAGE_SHOPPING;
    } else if (strcmp(key, "settings") == 0) {
        *out = FRIDGE_UI_PAGE_SETTINGS;
    } else if (strcmp(key, "wifi") == 0) {
        *out = FRIDGE_UI_PAGE_WIFI;
    } else if (strcmp(key, "more") == 0) {
        *out = FRIDGE_UI_PAGE_MORE;
    } else if (strcmp(key, "offline") == 0) {
        *out = FRIDGE_UI_PAGE_OFFLINE;
    } else if (strcmp(key, "ai") == 0) {
        *out = FRIDGE_UI_PAGE_AI;
    } else if (strcmp(key, "timer") == 0) {
        *out = FRIDGE_UI_PAGE_TIMER;
    } else if (strcmp(key, "stopwatch") == 0) {
        *out = FRIDGE_UI_PAGE_STOPWATCH;
    } else if (strcmp(key, "alarm") == 0) {
        *out = FRIDGE_UI_PAGE_ALARM;
    } else {
        return false;
    }
    return true;
}

esp_err_t fridge_ui_set_page_async(fridge_ui_page_t page, const char *toast)
{
    if (page >= FRIDGE_UI_PAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ui_task || !g_ui_root) {
        return ESP_ERR_INVALID_STATE;
    }

    ui_page_async_request_t *request = calloc(1, sizeof(*request));
    if (!request) {
        return ESP_ERR_NO_MEM;
    }
    request->page = page;
    strlcpy(request->toast, toast ? toast : "", sizeof(request->toast));
    if (lv_async_call(ui_set_page_async_cb, request) != LV_RESULT_OK) {
        free(request);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ai_actions_ui_page_handler(const char *page_key, const char *toast)
{
    fridge_ui_page_t page = FRIDGE_UI_PAGE_HOME;
    if (!fridge_ui_page_from_key(page_key, &page)) {
        return ESP_ERR_INVALID_ARG;
    }
    return fridge_ui_set_page_async(page, toast);
}

esp_err_t fridge_ui_init(void)
{
    if (s_ui_task) {
        return ESP_OK;
    }
    fridge_ai_actions_register_ui_page_handler(ai_actions_ui_page_handler);
    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "fridge_ui", UI_TASK_STACK, NULL, UI_TASK_PRIORITY, &s_ui_task, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t fridge_ui_set_page(fridge_ui_page_t page)
{
    if (page >= FRIDGE_UI_PAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    fridge_ui_show_page(page);
    return ESP_OK;
}

esp_err_t fridge_ui_set_brightness(uint8_t percent)
{
    return fridge_display_set_brightness(percent);
}

void fridge_ui_request_standby(void)
{
    fridge_ui_show_page(FRIDGE_UI_PAGE_STANDBY);
}

void fridge_ui_show_page(fridge_ui_page_t page)
{
    if (!s_pages[page]) {
        return;
    }
    if (g_ui_page < FRIDGE_UI_PAGE_COUNT && s_pages[g_ui_page] && g_ui_page != page) {
        if (g_ui_page == FRIDGE_UI_PAGE_CAMERA) {
            fridge_ui_page_camera_stop();
        }
        lv_obj_add_flag(s_pages[g_ui_page], LV_OBJ_FLAG_HIDDEN);
    }
    g_ui_page = page;
    lv_obj_remove_flag(s_pages[page], LV_OBJ_FLAG_HIDDEN);
    if (!s_auto_standby_active) {
        fridge_ui_note_activity();
    }
    update_current_page();
}

void fridge_ui_go_back(void)
{
    if (fridge_ui_model_is_place_picking()) {
        fridge_ui_model_cancel_place_pick();
        fridge_ui_show_page(FRIDGE_UI_PAGE_EDIT_FOOD);
        return;
    }
    switch (g_ui_page) {
    case FRIDGE_UI_PAGE_ZONE:
    case FRIDGE_UI_PAGE_DOOR:
    case FRIDGE_UI_PAGE_RECIPE:
    case FRIDGE_UI_PAGE_NUTRITION:
    case FRIDGE_UI_PAGE_AI:
    case FRIDGE_UI_PAGE_MORE:
    case FRIDGE_UI_PAGE_OFFLINE:
    case FRIDGE_UI_PAGE_TIMER:
    case FRIDGE_UI_PAGE_STOPWATCH:
    case FRIDGE_UI_PAGE_ALARM:
        fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
        break;
    case FRIDGE_UI_PAGE_EDIT_FOOD:
        fridge_ui_show_page(FRIDGE_UI_PAGE_ZONE);
        break;
    case FRIDGE_UI_PAGE_CAMERA:
        fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
        break;
    case FRIDGE_UI_PAGE_CAMERA_RESULT:
        fridge_ui_show_page(FRIDGE_UI_PAGE_CAMERA);
        break;
    case FRIDGE_UI_PAGE_SETTINGS:
        fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
        break;
    case FRIDGE_UI_PAGE_WIFI:
        fridge_ui_show_page(FRIDGE_UI_PAGE_SETTINGS);
        break;
    default:
        fridge_ui_show_page(FRIDGE_UI_PAGE_HOME);
        break;
    }
}

void fridge_ui_toast(const char *text)
{
    if (!s_toast || !text) {
        return;
    }
    fridge_ui_label_set_text_if_changed(s_toast, text);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_until_ms = esp_timer_get_time() / 1000 + UI_TOAST_MS;
}

void fridge_ui_note_activity(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000;
    s_touch_wake_until_ms = s_last_activity_ms + UI_TOUCH_WAKE_GRACE_MS;
    // 触摸是明确的人机互动：即使雷达缺席或仍处于 2 分钟无人状态，也给用户一个完整操作窗口。
    s_last_radar_seen_ms = s_last_activity_ms;
    if (fridge_display_get_brightness() == 0) {
        (void)fridge_display_set_brightness(65);
    }
}

void fridge_ui_label_set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) {
        return;
    }
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

void fridge_ui_label_set_text_fmt_if_changed(lv_obj_t *label, const char *fmt, ...)
{
    if (!label || !fmt) {
        return;
    }
    char buf[160] = {0};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fridge_ui_label_set_text_if_changed(label, buf);
}
