// 冰箱小精灵 UI 数据模型。
// UI 任务定期把 sensors/radar/network/storage 的最新状态复制到本地镜像，页面只读模型，避免直接阻塞硬件组件。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "fridge_display.h"
#include "fridge_mqtt_protocol.h"
#include "fridge_network.h"
#include "fridge_sensors.h"
#include "fridge_speaker.h"
#include "fridge_state_machine.h"
#include "fridge_storage.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "fridge_ui_model";

static SemaphoreHandle_t s_lock;
static fridge_ui_model_t s_model;
static TaskHandle_t s_wifi_scan_task;
static TaskHandle_t s_wifi_connect_task;
static char s_connect_ssid[FRIDGE_WIFI_MAX_SSID_LEN + 1];
static char s_connect_password[FRIDGE_WIFI_MAX_PASSWORD_LEN + 1];
static char s_inventory_buf[FRIDGE_STORAGE_MAX_JSON_LEN];
static bool s_place_picking;
static uint8_t s_place_pick_origin_zone = 1;
static uint8_t s_place_pick_origin_cell = 4;

static const char *CELL_NAMES[FRIDGE_UI_ZONE_CELL_COUNT] = {"A1", "A2", "A3", "B1", "B2", "B3", "C1", "C2", "C3"};

static uint8_t location_to_zone(const char *location)
{
    if (!location) {
        return 1;
    }
    if (strstr(location, "冷冻")) {
        return 0;
    }
    if (strstr(location, "门架")) {
        return 3;
    }
    if (strstr(location, "右")) {
        return 2;
    }
    return 1;
}

static uint8_t location_to_cell(const char *location, size_t fallback)
{
    if (location) {
        for (uint8_t i = 0; i < FRIDGE_UI_ZONE_CELL_COUNT; i++) {
            if (strstr(location, CELL_NAMES[i])) {
                return i;
            }
        }
    }
    return (uint8_t)(fallback % FRIDGE_UI_ZONE_CELL_COUNT);
}

static uint8_t food_days_from_expire_text(const char *text)
{
    if (!text || text[0] == '\0') {
        return 7;
    }
    if (strstr(text, "今天")) {
        return 0;
    }
    if (strstr(text, "明天")) {
        return 1;
    }
    int days = atoi(text);
    if (days < 0) {
        return 0;
    }
    if (days > 99) {
        return 99;
    }
    return (uint8_t)days;
}

static void model_fill_default_zones(fridge_ui_zone_summary_t *zones)
{
    memset(zones, 0, sizeof(fridge_ui_zone_summary_t) * FRIDGE_UI_ZONE_COUNT);
    strlcpy(zones[0].name, "上层冷冻", sizeof(zones[0].name));
    strlcpy(zones[0].note, "冷冻", sizeof(zones[0].note));
    strlcpy(zones[1].name, "左侧冷藏", sizeof(zones[1].name));
    strlcpy(zones[1].note, "冷藏", sizeof(zones[1].note));
    strlcpy(zones[2].name, "右侧冷藏", sizeof(zones[2].name));
    strlcpy(zones[2].note, "冷藏", sizeof(zones[2].note));
    strlcpy(zones[3].name, "门架", sizeof(zones[3].name));
    strlcpy(zones[3].note, "门架", sizeof(zones[3].note));
    strlcpy(zones[4].name, "自定义区", sizeof(zones[4].name));
    strlcpy(zones[4].note, "可放食材", sizeof(zones[4].note));
    strlcpy(zones[5].name, "备用区", sizeof(zones[5].name));
    strlcpy(zones[5].note, "可放食材", sizeof(zones[5].note));
    for (uint8_t i = 0; i < FRIDGE_UI_ZONE_COUNT; i++) {
        zones[i].width = 1;
        zones[i].height = 1;
    }
    // 默认四区使用固定冰箱拓扑的 span 初始值：
    // 上层冷冻横跨左/中两列，左右冷藏各占下方一列两行，门架占右侧整列。
    zones[0].width = 2;
    zones[0].height = 1;
    zones[1].width = 1;
    zones[1].height = 2;
    zones[2].width = 1;
    zones[2].height = 2;
    zones[3].width = 1;
    zones[3].height = 3;
    zones[4].custom = true;
    zones[5].custom = true;
}

static void model_lock_copy(fridge_ui_model_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_model;
    xSemaphoreGive(s_lock);
}

static void model_recount_zone_items(fridge_ui_model_t *model)
{
    if (!model) {
        return;
    }
    for (uint8_t i = 0; i < FRIDGE_UI_ZONE_COUNT; i++) {
        model->zones[i].count = 0;
    }
    for (size_t i = 0; i < model->food_count && i < FRIDGE_UI_MAX_FOODS; i++) {
        uint8_t zone = model->foods[i].zone;
        if (zone < FRIDGE_UI_ZONE_COUNT && model->foods[i].name[0] != '\0') {
            model->zones[zone].count++;
        }
    }
}

static void parse_inventory_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "inventory json parse failed");
        return;
    }

    fridge_ui_zone_summary_t zones[FRIDGE_UI_ZONE_COUNT] = {0};
    model_fill_default_zones(zones);
    fridge_ui_food_t foods[FRIDGE_UI_MAX_FOODS] = {0};
    size_t food_count = 0;
    uint8_t active_zone = s_model.active_zone;

    const cJSON *zone_items = cJSON_GetObjectItemCaseSensitive(root, "zones");
    const cJSON *zone_item = NULL;
    cJSON_ArrayForEach(zone_item, zone_items) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(zone_item, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(zone_item, "name");
        const cJSON *note = cJSON_GetObjectItemCaseSensitive(zone_item, "note");
        const cJSON *width = cJSON_GetObjectItemCaseSensitive(zone_item, "width");
        const cJSON *height = cJSON_GetObjectItemCaseSensitive(zone_item, "height");
        const cJSON *custom = cJSON_GetObjectItemCaseSensitive(zone_item, "custom");
        if (!cJSON_IsNumber(id) || id->valueint < 0 || id->valueint >= FRIDGE_UI_ZONE_COUNT) {
            continue;
        }
        if (cJSON_IsString(name) && name->valuestring && name->valuestring[0]) {
            strlcpy(zones[id->valueint].name, name->valuestring, sizeof(zones[id->valueint].name));
        }
        if (cJSON_IsString(note) && note->valuestring && note->valuestring[0]) {
            strlcpy(zones[id->valueint].note, note->valuestring, sizeof(zones[id->valueint].note));
        }
        if (cJSON_IsNumber(width) && width->valueint >= 1 && width->valueint <= 3) {
            zones[id->valueint].width = (uint8_t)width->valueint;
        }
        if (cJSON_IsNumber(height) && height->valueint >= 1 && height->valueint <= 3) {
            zones[id->valueint].height = (uint8_t)height->valueint;
        }
        zones[id->valueint].custom = cJSON_IsBool(custom) ? cJSON_IsTrue(custom) : zones[id->valueint].custom;
    }

    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *quantity = cJSON_GetObjectItemCaseSensitive(item, "quantity");
        const cJSON *expire = cJSON_GetObjectItemCaseSensitive(item, "expire_date");
        const cJSON *days = cJSON_GetObjectItemCaseSensitive(item, "days_left");
        const cJSON *location = cJSON_GetObjectItemCaseSensitive(item, "location");
        const cJSON *zone_json = cJSON_GetObjectItemCaseSensitive(item, "zone");
        const cJSON *cell_json = cJSON_GetObjectItemCaseSensitive(item, "cell");
        const char *loc = cJSON_IsString(location) ? location->valuestring : "";
        uint8_t zone = cJSON_IsNumber(zone_json) ? (uint8_t)zone_json->valueint : location_to_zone(loc);
        if (zone >= FRIDGE_UI_ZONE_COUNT) {
            zone = 1;
        }
        if (zone < FRIDGE_UI_ZONE_COUNT) {
            zones[zone].count++;
        }
        if (food_count < FRIDGE_UI_MAX_FOODS) {
            fridge_ui_food_t *food = &foods[food_count];
            strlcpy(food->name, cJSON_IsString(name) ? name->valuestring : "未命名", sizeof(food->name));
            strlcpy(food->quantity, cJSON_IsString(quantity) ? quantity->valuestring : "-", sizeof(food->quantity));
            strlcpy(food->expire, cJSON_IsString(expire) ? expire->valuestring : "-", sizeof(food->expire));
            strlcpy(food->location, loc, sizeof(food->location));
            food->days_left = cJSON_IsNumber(days) ? days->valueint : food_days_from_expire_text(food->expire);
            food->zone = zone;
            food->cell = cJSON_IsNumber(cell_json) ? (uint8_t)cell_json->valueint : location_to_cell(loc, food_count);
            if (food->cell >= FRIDGE_UI_ZONE_CELL_COUNT) {
                food->cell = location_to_cell(loc, food_count);
            }
            food_count++;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_model.zones, zones, sizeof(zones));
    memcpy(s_model.foods, foods, sizeof(foods));
    s_model.food_count = food_count;
    if (active_zone >= FRIDGE_UI_ZONE_COUNT) {
        s_model.active_zone = 1;
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
}

static char *model_to_inventory_json(const fridge_ui_model_t *model)
{
    if (!model) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddNumberToObject(root, "snapshot_version", 1);
    cJSON_AddStringToObject(root, "source", "lvgl_ui");

    cJSON *zones = cJSON_AddArrayToObject(root, "zones");
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    if (!zones || !items) {
        cJSON_Delete(root);
        return NULL;
    }

    for (uint8_t i = 0; i < FRIDGE_UI_ZONE_COUNT; i++) {
        if (model->zones[i].name[0] == '\0') {
            continue;
        }
        cJSON *zone = cJSON_CreateObject();
        if (!zone) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddNumberToObject(zone, "id", i);
        cJSON_AddStringToObject(zone, "name", model->zones[i].name);
        cJSON_AddStringToObject(zone, "note", model->zones[i].note);
        cJSON_AddNumberToObject(zone, "width", model->zones[i].width ? model->zones[i].width : 1);
        cJSON_AddNumberToObject(zone, "height", model->zones[i].height ? model->zones[i].height : 1);
        cJSON_AddBoolToObject(zone, "custom", model->zones[i].custom);
        cJSON_AddItemToArray(zones, zone);
    }

    for (size_t i = 0; i < model->food_count && i < FRIDGE_UI_MAX_FOODS; i++) {
        const fridge_ui_food_t *food = &model->foods[i];
        if (food->name[0] == '\0') {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddStringToObject(item, "name", food->name);
        cJSON_AddStringToObject(item, "quantity", food->quantity);
        cJSON_AddStringToObject(item, "expire_date", food->expire);
        cJSON_AddNumberToObject(item, "days_left", food->days_left);
        cJSON_AddStringToObject(item, "location", food->location);
        cJSON_AddNumberToObject(item, "zone", food->zone);
        cJSON_AddNumberToObject(item, "cell", food->cell);
        cJSON_AddItemToArray(items, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void model_persist_snapshot(const fridge_ui_model_t *model)
{
    char *json = model_to_inventory_json(model);
    if (!json) {
        ESP_LOGW(TAG, "serialize ui inventory failed");
        return;
    }
    esp_err_t ret = fridge_storage_set_ui_inventory_snapshot(json);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "persist ui inventory failed: %s", esp_err_to_name(ret));
    } else {
        // 用户确认修改后立即尝试上报完整库存快照；离线时保留 dirty 标记，等待下次同步刷新。
        esp_err_t publish_ret = fridge_mqtt_publish_inventory_snapshot(false);
        if (publish_ret != ESP_OK && publish_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "publish ui inventory snapshot failed: %s", esp_err_to_name(publish_ret));
        }
    }
    cJSON_free(json);
}

static void wifi_scan_task(void *arg)
{
    (void)arg;
    fridge_wifi_ap_t aps[FRIDGE_UI_MAX_WIFI_APS] = {0};
    size_t count = 0;
    esp_err_t ret = fridge_network_scan(aps, FRIDGE_UI_MAX_WIFI_APS, &count);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_model.wifi_scanning = false;
    s_model.wifi_ap_count = 0;
    if (ret == ESP_OK) {
        for (size_t i = 0; i < count && i < FRIDGE_UI_MAX_WIFI_APS; i++) {
            strlcpy(s_model.wifi_aps[i].ssid, aps[i].ssid, sizeof(s_model.wifi_aps[i].ssid));
            strlcpy(s_model.wifi_aps[i].authmode, aps[i].authmode, sizeof(s_model.wifi_aps[i].authmode));
            s_model.wifi_aps[i].rssi = aps[i].rssi;
            s_model.wifi_aps[i].secured = aps[i].secured;
            s_model.wifi_ap_count++;
        }
        snprintf(s_model.wifi_status, sizeof(s_model.wifi_status), "扫描完成：%u 个热点", (unsigned)s_model.wifi_ap_count);
    } else {
        snprintf(s_model.wifi_status, sizeof(s_model.wifi_status), "扫描失败：%s", esp_err_to_name(ret));
    }
    xSemaphoreGive(s_lock);

    s_wifi_scan_task = NULL;
    vTaskDelete(NULL);
}

static void wifi_connect_task(void *arg)
{
    (void)arg;
    fridge_wifi_config_t config = {0};
    strlcpy(config.ssid, s_connect_ssid, sizeof(config.ssid));
    strlcpy(config.password, s_connect_password, sizeof(config.password));
    esp_err_t ret = fridge_network_connect(&config, true);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_model.wifi_status,
             sizeof(s_model.wifi_status),
             ret == ESP_OK ? "已连接 %s" : "连接失败：%s",
             ret == ESP_OK ? config.ssid : esp_err_to_name(ret));
    xSemaphoreGive(s_lock);

    s_wifi_connect_task = NULL;
    vTaskDelete(NULL);
}

void fridge_ui_model_init(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    memset(&s_model, 0, sizeof(s_model));
    model_fill_default_zones(s_model.zones);
    s_model.active_zone = 1;
    s_model.active_cell = 4;
    s_model.brightness = fridge_display_get_brightness();
    s_model.speaker_volume = fridge_speaker_get_volume();
    strlcpy(s_model.wifi_status, "点击刷新扫描热点", sizeof(s_model.wifi_status));
}

void fridge_ui_model_poll(void)
{
    fridge_sensor_snapshot_t sensors = {0};
    if (fridge_sensors_get_snapshot(&sensors) == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_model.sensors.ready = sensors.ready;
        s_model.sensors.light_percent = sensors.light_percent;
        s_model.sensors.radar_presence = sensors.radar.presence;
        s_model.sensors.radar_stable_presence = sensors.radar.stable_presence;
        s_model.sensors.updated_at_ms = sensors.updated_at_ms;
        xSemaphoreGive(s_lock);
    }

    fridge_sm_snapshot_t sm = {0};
    fridge_sm_config_t sm_config = {0};
    if (fridge_state_machine_get_snapshot(&sm) == ESP_OK) {
        (void)fridge_state_machine_get_config(&sm_config);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_model.sensors.state_machine_state,
                fridge_state_machine_state_to_string(sm.state),
                sizeof(s_model.sensors.state_machine_state));
        strlcpy(s_model.sensors.door_state,
                fridge_state_machine_door_to_string(sm.door_state),
                sizeof(s_model.sensors.door_state));
        s_model.sensors.state_offline = sm.offline;
        s_model.sensors.state_is_night = sm.is_night;
        s_model.sensors.radar_within_2m = sm.radar_within_2m;
        s_model.sensors.auto_voice_after_close = sm_config.auto_voice_after_close;
        s_model.sensors.updated_at_ms = sm.updated_at_ms;
        xSemaphoreGive(s_lock);
    }

    fridge_network_status_t net = {0};
    if (fridge_network_get_status(&net) == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_model.network.connected = net.connected;
        s_model.network.connecting = net.connecting;
        s_model.network.internet_ready = net.internet_ready;
        s_model.network.rssi = net.rssi;
        strlcpy(s_model.network.ssid, net.ssid, sizeof(s_model.network.ssid));
        strlcpy(s_model.network.ip, net.ip, sizeof(s_model.network.ip));
        strlcpy(s_model.network.last_error, net.last_error, sizeof(s_model.network.last_error));
        xSemaphoreGive(s_lock);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_model.brightness = fridge_display_get_brightness();
    s_model.speaker_volume = fridge_speaker_get_volume();
    xSemaphoreGive(s_lock);

    if (fridge_storage_get_ui_inventory_snapshot(s_inventory_buf, sizeof(s_inventory_buf)) == ESP_OK) {
        parse_inventory_json(s_inventory_buf);
    }
}

void fridge_ui_model_get(fridge_ui_model_t *out)
{
    if (!out || !s_lock) {
        return;
    }
    model_lock_copy(out);
}

void fridge_ui_model_set_active_zone(uint8_t zone)
{
    if (zone >= FRIDGE_UI_ZONE_COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_model.active_zone = zone;
    xSemaphoreGive(s_lock);
}

void fridge_ui_model_select_cell(uint8_t zone, uint8_t cell)
{
    if (zone >= FRIDGE_UI_ZONE_COUNT || cell >= FRIDGE_UI_ZONE_CELL_COUNT) {
        return;
    }

    fridge_ui_model_t snapshot = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_model.active_zone = zone;
    s_model.active_cell = cell;
    s_model.editing_valid = false;
    memset(&s_model.editing_food, 0, sizeof(s_model.editing_food));
    for (size_t i = 0; i < s_model.food_count; i++) {
        if (s_model.foods[i].zone == zone && s_model.foods[i].cell == cell) {
            s_model.editing_food = s_model.foods[i];
            s_model.editing_valid = true;
            break;
        }
    }
    if (!s_model.editing_valid) {
        strlcpy(s_model.editing_food.name, "", sizeof(s_model.editing_food.name));
        strlcpy(s_model.editing_food.quantity, "1份", sizeof(s_model.editing_food.quantity));
        strlcpy(s_model.editing_food.expire, "3天后", sizeof(s_model.editing_food.expire));
        snprintf(s_model.editing_food.location,
                 sizeof(s_model.editing_food.location),
                 "%s %s",
                 s_model.zones[zone].name,
                 CELL_NAMES[cell]);
        s_model.editing_food.days_left = 3;
        s_model.editing_food.zone = zone;
        s_model.editing_food.cell = cell;
    }
    snapshot = s_model;
    xSemaphoreGive(s_lock);
    (void)snapshot;
}

void fridge_ui_model_set_editing_draft(const fridge_ui_food_t *food)
{
    if (!food || food->zone >= FRIDGE_UI_ZONE_COUNT || food->cell >= FRIDGE_UI_ZONE_CELL_COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_model.editing_food = *food;
    s_model.editing_valid = food->name[0] != '\0';
    s_model.active_zone = food->zone;
    s_model.active_cell = food->cell;
    xSemaphoreGive(s_lock);
}

void fridge_ui_model_begin_place_pick(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_place_picking = true;
    s_place_pick_origin_zone = s_model.editing_food.zone;
    s_place_pick_origin_cell = s_model.editing_food.cell;
    if (s_place_pick_origin_zone >= FRIDGE_UI_ZONE_COUNT) {
        s_place_pick_origin_zone = s_model.active_zone;
    }
    if (s_place_pick_origin_cell >= FRIDGE_UI_ZONE_CELL_COUNT) {
        s_place_pick_origin_cell = s_model.active_cell;
    }
    xSemaphoreGive(s_lock);
}

void fridge_ui_model_cancel_place_pick(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_place_picking = false;
    xSemaphoreGive(s_lock);
}

bool fridge_ui_model_is_place_picking(void)
{
    bool picking = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    picking = s_place_picking;
    xSemaphoreGive(s_lock);
    return picking;
}

bool fridge_ui_model_apply_place_pick(uint8_t zone, uint8_t cell)
{
    if (zone >= FRIDGE_UI_ZONE_COUNT || cell >= FRIDGE_UI_ZONE_CELL_COUNT) {
        return false;
    }

    fridge_ui_model_t snapshot = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_model.active_zone = zone;
    s_model.active_cell = cell;
    s_model.editing_food.zone = zone;
    s_model.editing_food.cell = cell;
    snprintf(s_model.editing_food.location,
             sizeof(s_model.editing_food.location),
             "%s %s",
             s_model.zones[zone].name,
             CELL_NAMES[cell]);

    if (s_place_pick_origin_zone < FRIDGE_UI_ZONE_COUNT && s_place_pick_origin_cell < FRIDGE_UI_ZONE_CELL_COUNT &&
        (s_place_pick_origin_zone != zone || s_place_pick_origin_cell != cell)) {
        for (size_t i = 0; i < s_model.food_count; i++) {
            if (s_model.foods[i].zone == s_place_pick_origin_zone && s_model.foods[i].cell == s_place_pick_origin_cell) {
                memmove(&s_model.foods[i], &s_model.foods[i + 1], (s_model.food_count - i - 1) * sizeof(s_model.foods[0]));
                s_model.food_count--;
                break;
            }
        }
    }

    bool found = false;
    for (size_t i = 0; i < s_model.food_count; i++) {
        if (s_model.foods[i].zone == zone && s_model.foods[i].cell == cell) {
            s_model.foods[i] = s_model.editing_food;
            found = true;
            break;
        }
    }
    if (!found && s_model.editing_food.name[0] != '\0' && s_model.food_count < FRIDGE_UI_MAX_FOODS) {
        s_model.foods[s_model.food_count++] = s_model.editing_food;
    }
    s_model.editing_valid = s_model.editing_food.name[0] != '\0';
    s_place_picking = false;
    model_recount_zone_items(&s_model);
    snapshot = s_model;
    xSemaphoreGive(s_lock);
    model_persist_snapshot(&snapshot);
    return true;
}

void fridge_ui_model_update_editing_food(const fridge_ui_food_t *food)
{
    if (!food || food->zone >= FRIDGE_UI_ZONE_COUNT || food->cell >= FRIDGE_UI_ZONE_CELL_COUNT) {
        return;
    }
    fridge_ui_model_t snapshot = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    for (size_t i = 0; i < s_model.food_count; i++) {
        if (s_model.foods[i].zone == food->zone && s_model.foods[i].cell == food->cell) {
            s_model.foods[i] = *food;
            found = true;
            break;
        }
    }
    if (!found && food->name[0] != '\0' && s_model.food_count < FRIDGE_UI_MAX_FOODS) {
        s_model.foods[s_model.food_count++] = *food;
    }
    s_model.editing_food = *food;
    s_model.editing_valid = food->name[0] != '\0';
    model_recount_zone_items(&s_model);
    snapshot = s_model;
    xSemaphoreGive(s_lock);
    model_persist_snapshot(&snapshot);
}

void fridge_ui_model_delete_editing_food(void)
{
    fridge_ui_model_t snapshot = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t zone = s_model.editing_food.zone;
    uint8_t cell = s_model.editing_food.cell;
    if (!s_model.editing_valid) {
        zone = s_model.active_zone;
        cell = s_model.active_cell;
    }
    for (size_t i = 0; i < s_model.food_count; i++) {
        if (s_model.foods[i].zone == zone && s_model.foods[i].cell == cell) {
            memmove(&s_model.foods[i], &s_model.foods[i + 1], (s_model.food_count - i - 1) * sizeof(s_model.foods[0]));
            s_model.food_count--;
            break;
        }
    }
    memset(&s_model.editing_food, 0, sizeof(s_model.editing_food));
    s_model.editing_valid = false;
    model_recount_zone_items(&s_model);
    snapshot = s_model;
    xSemaphoreGive(s_lock);
    model_persist_snapshot(&snapshot);
}

void fridge_ui_model_add_camera_food(const char *name, const char *quantity, uint8_t zone, uint8_t cell)
{
    if (zone >= FRIDGE_UI_ZONE_COUNT || cell >= FRIDGE_UI_ZONE_CELL_COUNT) {
        zone = 1;
        cell = 4;
    }
    fridge_ui_food_t food = {0};
    strlcpy(food.name, name && name[0] ? name : "待确认食材", sizeof(food.name));
    strlcpy(food.quantity, quantity && quantity[0] ? quantity : "1份", sizeof(food.quantity));
    strlcpy(food.expire, "3天后", sizeof(food.expire));
    food.days_left = 3;
    food.zone = zone;
    food.cell = cell;
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    snprintf(food.location, sizeof(food.location), "%s %s", model.zones[zone].name, CELL_NAMES[cell]);
    fridge_ui_model_update_editing_food(&food);
}

void fridge_ui_model_set_camera_result(const fridge_ui_camera_result_t *result)
{
    if (!result) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_model.camera_result = *result;
    xSemaphoreGive(s_lock);
}

void fridge_ui_model_clear_camera_result(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_model.camera_result, 0, sizeof(s_model.camera_result));
    xSemaphoreGive(s_lock);
}

bool fridge_ui_model_add_custom_zone(void)
{
    fridge_ui_model_t snapshot = {0};
    bool added = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint8_t i = 4; i < FRIDGE_UI_ZONE_COUNT; i++) {
        if (s_model.zones[i].custom && s_model.zones[i].count == 0 &&
            (strcmp(s_model.zones[i].name, "自定义区") == 0 || strcmp(s_model.zones[i].name, "备用区") == 0)) {
            snprintf(s_model.zones[i].name, sizeof(s_model.zones[i].name), "自定义区 %u", (unsigned)(i - 3));
            strlcpy(s_model.zones[i].note, "可放食材", sizeof(s_model.zones[i].note));
            s_model.zones[i].width = 1;
            s_model.zones[i].height = 1;
            s_model.zones[i].custom = true;
            s_model.active_zone = i;
            added = true;
            break;
        }
    }
    if (!added) {
        for (uint8_t i = 4; i < FRIDGE_UI_ZONE_COUNT; i++) {
            if (s_model.zones[i].custom) {
                s_model.active_zone = i;
                break;
            }
        }
    }
    snapshot = s_model;
    xSemaphoreGive(s_lock);
    if (added) {
        model_persist_snapshot(&snapshot);
    }
    return added;
}

bool fridge_ui_model_rename_active_zone(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    fridge_ui_model_t snapshot = {0};
    bool renamed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t zone = s_model.active_zone;
    if (zone >= 4 && zone < FRIDGE_UI_ZONE_COUNT && s_model.zones[zone].custom) {
        strlcpy(s_model.zones[zone].name, name, sizeof(s_model.zones[zone].name));
        for (size_t i = 0; i < s_model.food_count; i++) {
            if (s_model.foods[i].zone == zone) {
                snprintf(s_model.foods[i].location,
                         sizeof(s_model.foods[i].location),
                         "%s %s",
                         s_model.zones[zone].name,
                         CELL_NAMES[s_model.foods[i].cell]);
            }
        }
        renamed = true;
    }
    snapshot = s_model;
    xSemaphoreGive(s_lock);
    if (renamed) {
        model_persist_snapshot(&snapshot);
    }
    return renamed;
}

bool fridge_ui_model_update_zone(uint8_t zone, const char *name, uint8_t width, uint8_t height, const char *note)
{
    if (zone >= FRIDGE_UI_ZONE_COUNT || !name || name[0] == '\0') {
        return false;
    }
    if (width < 1) {
        width = 1;
    } else if (width > 3) {
        width = 3;
    }
    if (height < 1) {
        height = 1;
    } else if (height > 3) {
        height = 3;
    }

    fridge_ui_model_t snapshot = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_model.zones[zone].name, name, sizeof(s_model.zones[zone].name));
    strlcpy(s_model.zones[zone].note, note && note[0] ? note : "可放食材", sizeof(s_model.zones[zone].note));
    s_model.zones[zone].width = width;
    s_model.zones[zone].height = height;
    for (size_t i = 0; i < s_model.food_count; i++) {
        if (s_model.foods[i].zone == zone) {
            snprintf(s_model.foods[i].location,
                     sizeof(s_model.foods[i].location),
                     "%s %s",
                     s_model.zones[zone].name,
                     CELL_NAMES[s_model.foods[i].cell]);
        }
    }
    snapshot = s_model;
    xSemaphoreGive(s_lock);
    model_persist_snapshot(&snapshot);
    return true;
}

void fridge_ui_model_delete_zone(uint8_t zone)
{
    fridge_ui_model_t snapshot = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (zone >= 4 && zone < FRIDGE_UI_ZONE_COUNT && s_model.zones[zone].custom) {
        memset(&s_model.zones[zone], 0, sizeof(s_model.zones[zone]));
        snprintf(s_model.zones[zone].name, sizeof(s_model.zones[zone].name), "自定义区");
        strlcpy(s_model.zones[zone].note, "可放食材", sizeof(s_model.zones[zone].note));
        s_model.zones[zone].width = 1;
        s_model.zones[zone].height = 1;
        s_model.zones[zone].custom = true;
        for (size_t i = 0; i < s_model.food_count;) {
            if (s_model.foods[i].zone == zone) {
                memmove(&s_model.foods[i], &s_model.foods[i + 1], (s_model.food_count - i - 1) * sizeof(s_model.foods[0]));
                s_model.food_count--;
                continue;
            }
            i++;
        }
        s_model.active_zone = 1;
    }
    snapshot = s_model;
    xSemaphoreGive(s_lock);
    model_persist_snapshot(&snapshot);
}

void fridge_ui_model_delete_active_custom_zone(void)
{
    fridge_ui_model_t model = {0};
    fridge_ui_model_get(&model);
    fridge_ui_model_delete_zone(model.active_zone);
}

void fridge_ui_model_start_wifi_scan(void)
{
    if (s_wifi_scan_task) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_model.wifi_scanning = true;
    strlcpy(s_model.wifi_status, "正在扫描 2.4GHz 热点...", sizeof(s_model.wifi_status));
    xSemaphoreGive(s_lock);
    if (xTaskCreate(wifi_scan_task, "ui_wifi_scan", 4096, NULL, 4, &s_wifi_scan_task) != pdPASS) {
        s_wifi_scan_task = NULL;
    }
}

void fridge_ui_model_connect_wifi_async(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || s_wifi_connect_task) {
        return;
    }
    strlcpy(s_connect_ssid, ssid, sizeof(s_connect_ssid));
    strlcpy(s_connect_password, password ? password : "", sizeof(s_connect_password));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_model.wifi_status, sizeof(s_model.wifi_status), "正在连接 %s...", s_connect_ssid);
    xSemaphoreGive(s_lock);
    if (xTaskCreate(wifi_connect_task, "ui_wifi_conn", 4096, NULL, 4, &s_wifi_connect_task) != pdPASS) {
        s_wifi_connect_task = NULL;
    }
}
