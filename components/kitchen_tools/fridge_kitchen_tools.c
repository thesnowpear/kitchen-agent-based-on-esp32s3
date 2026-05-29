// 冰箱小精灵厨房计时工具组件。
// 负责定时器、秒表和闹钟的本地状态管理；计时使用 esp_timer 单调时钟，闹钟使用已校准系统时间。
// 硬件注意：本组件不配置 GPIO/I2S，只在到点时调用 speaker 组件；扬声器播放时仍需稳定 5V/2A 供电。

#include "fridge_kitchen_tools.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_speaker.h"
#include "fridge_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define KITCHEN_TOOLS_TASK_STACK 6144
#define KITCHEN_TOOLS_TASK_PRIORITY 3
#define KITCHEN_TOOLS_POLL_MS 500
#define KITCHEN_TOOLS_EPOCH_READY_THRESHOLD 1735689600LL
#define KITCHEN_TOOLS_ALARM_PATH "/cache/kitchen_alarms.json"
#define KITCHEN_TOOLS_ALARM_TMP_PATH "/cache/kitchen_alarms.tmp"

static const char *TAG = "fridge_kitchen_tools";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static bool s_initialized;
static fridge_kitchen_timer_snapshot_t s_timer;
static fridge_kitchen_stopwatch_snapshot_t s_stopwatch;
static fridge_kitchen_alarm_t s_alarms[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
static size_t s_alarm_count;
static char s_last_alert[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
static char s_last_error[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool time_ready(void)
{
    return time(NULL) >= KITCHEN_TOOLS_EPOCH_READY_THRESHOLD;
}

static size_t utf8_valid_prefix_len(const char *text)
{
    if (!text) {
        return 0;
    }
    const unsigned char *bytes = (const unsigned char *)text;
    size_t len = strlen(text);
    size_t pos = 0;
    while (pos < len) {
        unsigned char ch = bytes[pos];
        size_t need = 1;
        if (ch < 0x80) {
            need = 1;
        } else if ((ch & 0xE0) == 0xC0) {
            need = 2;
        } else if ((ch & 0xF0) == 0xE0) {
            need = 3;
        } else if ((ch & 0xF8) == 0xF0) {
            need = 4;
        } else {
            break;
        }
        if (pos + need > len) {
            break;
        }
        for (size_t i = 1; i < need; i++) {
            if ((bytes[pos + i] & 0xC0) != 0x80) {
                return pos;
            }
        }
        pos += need;
    }
    return pos;
}

static int64_t local_day_key(const struct tm *tm_now)
{
    if (!tm_now) {
        return 0;
    }
    return ((int64_t)(tm_now->tm_year + 1900) * 10000) + ((int64_t)(tm_now->tm_mon + 1) * 100) + tm_now->tm_mday;
}

static void copy_label(char *out, size_t out_size, const char *label, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }
    const char *source = (label && label[0]) ? label : fallback;
    strlcpy(out, source ? source : "", out_size);
    // 标签按字节存储，截断中文时需要回退到完整 UTF-8 边界，避免持久化 JSON 或 USB 输出出现乱码。
    out[utf8_valid_prefix_len(out)] = '\0';
}

static bool json_key_allowed(const char *key)
{
    static const char *allowed[] = {
        "tool",
        "action",
        "duration_seconds",
        "hour",
        "minute",
        "label",
        "id",
    };
    if (!key) {
        return false;
    }
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(key, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *kitchen_err_text(esp_err_t err, const char *tool, const char *action)
{
    if (err == ESP_ERR_INVALID_STATE && tool && strcmp(tool, "alarm") == 0 && action && strcmp(action, "set") == 0) {
        return "时间未同步，暂不能设置闹钟";
    }
    if (err == ESP_ERR_INVALID_STATE && tool && strcmp(tool, "timer") == 0) {
        return "当前定时器状态不支持这个操作";
    }
    if (err == ESP_ERR_INVALID_STATE && tool && strcmp(tool, "stopwatch") == 0) {
        return "当前秒表状态不支持这个操作";
    }
    if (err == ESP_ERR_NOT_FOUND && tool && strcmp(tool, "alarm") == 0) {
        return "没有找到这个闹钟";
    }
    if (err == ESP_ERR_NO_MEM && tool && strcmp(tool, "alarm") == 0) {
        return "闹钟数量已满，最多 5 个";
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return "厨房工具指令参数无效";
    }
    return esp_err_to_name(err);
}

static void set_last_error(const char *message)
{
    strlcpy(s_last_error, message ? message : "", sizeof(s_last_error));
}

static uint32_t timer_remaining_locked(int64_t now)
{
    if (s_timer.state == FRIDGE_KITCHEN_TIMER_RUNNING) {
        if (now >= s_timer.target_at_ms) {
            return 0;
        }
        int64_t left_ms = s_timer.target_at_ms - now;
        return (uint32_t)((left_ms + 999) / 1000);
    }
    return s_timer.remaining_seconds;
}

static uint32_t stopwatch_elapsed_locked(int64_t now)
{
    if (s_stopwatch.state == FRIDGE_KITCHEN_STOPWATCH_RUNNING) {
        uint32_t base = s_stopwatch.elapsed_seconds;
        if (now > s_stopwatch.started_at_ms) {
            base += (uint32_t)((now - s_stopwatch.started_at_ms) / 1000);
        }
        return base;
    }
    return s_stopwatch.elapsed_seconds;
}

static void fill_snapshot_locked(fridge_kitchen_tools_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->time_ready = time_ready();
    int64_t now = now_ms();
    out->timer = s_timer;
    out->timer.remaining_seconds = timer_remaining_locked(now);
    out->stopwatch = s_stopwatch;
    out->stopwatch.elapsed_seconds = stopwatch_elapsed_locked(now);
    out->alarm_count = s_alarm_count;
    for (size_t i = 0; i < s_alarm_count && i < FRIDGE_KITCHEN_TOOL_MAX_ALARMS; i++) {
        out->alarms[i] = s_alarms[i];
    }
    strlcpy(out->last_alert, s_last_alert, sizeof(out->last_alert));
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
}

static esp_err_t save_alarms_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create alarm root failed");
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON *items = cJSON_AddArrayToObject(root, "alarms");
    if (!items) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < s_alarm_count && i < FRIDGE_KITCHEN_TOOL_MAX_ALARMS; i++) {
        const fridge_kitchen_alarm_t *alarm = &s_alarms[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(item, "id", alarm->id);
        cJSON_AddBoolToObject(item, "enabled", alarm->enabled);
        cJSON_AddNumberToObject(item, "hour", alarm->hour);
        cJSON_AddNumberToObject(item, "minute", alarm->minute);
        cJSON_AddNumberToObject(item, "last_trigger_day", (double)alarm->last_trigger_day);
        cJSON_AddStringToObject(item, "label", alarm->label);
        cJSON_AddItemToArray(items, item);
    }

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print alarm json failed");

    FILE *file = fopen(KITCHEN_TOOLS_ALARM_TMP_PATH, "wb");
    if (!file) {
        cJSON_free(printed);
        return ESP_FAIL;
    }
    size_t len = strlen(printed);
    size_t written = fwrite(printed, 1, len, file);
    int close_ret = fclose(file);
    cJSON_free(printed);
    if (written != len || close_ret != 0) {
        remove(KITCHEN_TOOLS_ALARM_TMP_PATH);
        return ESP_FAIL;
    }
    if (rename(KITCHEN_TOOLS_ALARM_TMP_PATH, KITCHEN_TOOLS_ALARM_PATH) != 0) {
        remove(KITCHEN_TOOLS_ALARM_TMP_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool alarm_id_exists_locked(uint8_t id)
{
    if (id == 0) {
        return true;
    }
    for (size_t i = 0; i < s_alarm_count; i++) {
        if (s_alarms[i].id == id) {
            return true;
        }
    }
    return false;
}

static void load_alarms_locked(void)
{
    FILE *file = fopen(KITCHEN_TOOLS_ALARM_PATH, "rb");
    if (!file) {
        return;
    }
    char *buffer = calloc(1, 2048);
    if (!buffer) {
        fclose(file);
        return;
    }
    size_t bytes = fread(buffer, 1, 2047, file);
    fclose(file);
    buffer[bytes] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) {
        ESP_LOGW(TAG, "alarm file invalid, ignore");
        return;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "alarms");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return;
    }

    s_alarm_count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (s_alarm_count >= FRIDGE_KITCHEN_TOOL_MAX_ALARMS) {
            break;
        }
        const cJSON *hour = cJSON_GetObjectItemCaseSensitive(item, "hour");
        const cJSON *minute = cJSON_GetObjectItemCaseSensitive(item, "minute");
        if (!cJSON_IsNumber(hour) || !cJSON_IsNumber(minute) || hour->valueint < 0 || hour->valueint > 23 ||
            minute->valueint < 0 || minute->valueint > 59) {
            continue;
        }
        fridge_kitchen_alarm_t *alarm = &s_alarms[s_alarm_count];
        memset(alarm, 0, sizeof(*alarm));
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(item, "label");
        const cJSON *last_day = cJSON_GetObjectItemCaseSensitive(item, "last_trigger_day");
        uint8_t loaded_id = 0;
        if (cJSON_IsNumber(id) && id->valueint > 0 && id->valueint < 250) {
            loaded_id = (uint8_t)id->valueint;
        }
        if (loaded_id == 0 || alarm_id_exists_locked(loaded_id)) {
            loaded_id = (uint8_t)(s_alarm_count + 1);
            while (alarm_id_exists_locked(loaded_id) && loaded_id < 250) {
                loaded_id++;
            }
        }
        if (loaded_id == 0 || alarm_id_exists_locked(loaded_id)) {
            strlcpy(s_last_error, "跳过了无效闹钟编号", sizeof(s_last_error));
            continue;
        }
        alarm->id = loaded_id;
        alarm->enabled = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : true;
        alarm->hour = (uint8_t)hour->valueint;
        alarm->minute = (uint8_t)minute->valueint;
        alarm->last_trigger_day = cJSON_IsNumber(last_day) ? (int64_t)last_day->valuedouble : 0;
        if (cJSON_IsString(label) && label->valuestring) {
            copy_label(alarm->label, sizeof(alarm->label), label->valuestring, "厨房提醒");
        } else {
            copy_label(alarm->label, sizeof(alarm->label), NULL, "厨房提醒");
        }
        s_alarm_count++;
    }
    cJSON_Delete(root);
}

static void alert_task(void *arg)
{
    char *text = (char *)arg;
    if (!text) {
        vTaskDelete(NULL);
        return;
    }
    fridge_speaker_status_t status = {0};
    esp_err_t err = fridge_speaker_synthesize_and_play(text, &status);
    if (err != ESP_OK) {
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
            strlcpy(s_last_error, status.error[0] ? status.error : esp_err_to_name(err), sizeof(s_last_error));
            xSemaphoreGive(s_lock);
        } else {
            ESP_LOGW(TAG, "alert TTS failed and last_error lock timed out");
        }
        ESP_LOGW(TAG, "alert TTS failed: %s", status.error[0] ? status.error : esp_err_to_name(err));
    }
    free(text);
    vTaskDelete(NULL);
}

static void trigger_alert_locked(const char *message)
{
    strlcpy(s_last_alert, message ? message : "厨房提醒到了", sizeof(s_last_alert));
    char *tts_text = malloc(strlen(s_last_alert) + 1);
    if (!tts_text) {
        strlcpy(s_last_error, "提醒播报内存不足", sizeof(s_last_error));
        return;
    }
    strlcpy(tts_text, s_last_alert, strlen(s_last_alert) + 1);
    BaseType_t ok = xTaskCreate(alert_task, "kitchen_alert", KITCHEN_TOOLS_TASK_STACK, tts_text, KITCHEN_TOOLS_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        free(tts_text);
        strlcpy(s_last_error, "提醒播报任务创建失败", sizeof(s_last_error));
    }
}

static void kitchen_tools_task(void *arg)
{
    (void)arg;
    while (true) {
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            int64_t mono_now = now_ms();
            if (s_timer.state == FRIDGE_KITCHEN_TIMER_RUNNING && mono_now >= s_timer.target_at_ms) {
                s_timer.state = FRIDGE_KITCHEN_TIMER_RINGING;
                s_timer.remaining_seconds = 0;
                char message[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
                snprintf(message, sizeof(message), "%s定时器到了", s_timer.label[0] ? s_timer.label : "厨房");
                trigger_alert_locked(message);
            }

            if (time_ready()) {
                time_t wall_now = time(NULL);
                struct tm tm_now = {0};
                localtime_r(&wall_now, &tm_now);
                int64_t day = local_day_key(&tm_now);
                for (size_t i = 0; i < s_alarm_count; i++) {
                    fridge_kitchen_alarm_t *alarm = &s_alarms[i];
                    if (!alarm->enabled || alarm->ringing || alarm->last_trigger_day == day) {
                        continue;
                    }
                    if (tm_now.tm_hour == alarm->hour && tm_now.tm_min == alarm->minute) {
                        alarm->ringing = true;
                        alarm->last_trigger_day = day;
                        char message[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
                        snprintf(message, sizeof(message), "%s闹钟到了", alarm->label[0] ? alarm->label : "厨房");
                        trigger_alert_locked(message);
                        (void)save_alarms_locked();
                    }
                }
            }
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(KITCHEN_TOOLS_POLL_MS));
    }
}

esp_err_t fridge_kitchen_tools_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create kitchen tools mutex failed");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init before kitchen tools failed");

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        memset(&s_timer, 0, sizeof(s_timer));
        memset(&s_stopwatch, 0, sizeof(s_stopwatch));
        memset(s_alarms, 0, sizeof(s_alarms));
        s_alarm_count = 0;
        load_alarms_locked();
        s_initialized = true;
        xSemaphoreGive(s_lock);
    }

    BaseType_t ok = xTaskCreate(kitchen_tools_task, "kitchen_tools", KITCHEN_TOOLS_TASK_STACK, NULL, KITCHEN_TOOLS_TASK_PRIORITY, &s_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create kitchen tools task failed");
    ESP_LOGI(TAG, "kitchen tools initialized, alarms=%u", (unsigned)s_alarm_count);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_get_snapshot(fridge_kitchen_tools_snapshot_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "snapshot output is NULL");
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before snapshot failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "snapshot lock timeout");
    fill_snapshot_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_start(uint32_t seconds, const char *label)
{
    ESP_RETURN_ON_FALSE(seconds > 0 && seconds <= 24 * 60 * 60, ESP_ERR_INVALID_ARG, TAG, "timer seconds invalid");
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer start failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    int64_t now = now_ms();
    memset(&s_timer, 0, sizeof(s_timer));
    s_timer.state = FRIDGE_KITCHEN_TIMER_RUNNING;
    s_timer.duration_seconds = seconds;
    s_timer.remaining_seconds = seconds;
    s_timer.started_at_ms = now;
    s_timer.target_at_ms = now + ((int64_t)seconds * 1000);
    copy_label(s_timer.label, sizeof(s_timer.label), label, "厨房");
    set_last_error("");
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_pause(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer pause failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    if (s_timer.state != FRIDGE_KITCHEN_TIMER_RUNNING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_timer.remaining_seconds = timer_remaining_locked(now_ms());
    s_timer.state = FRIDGE_KITCHEN_TIMER_PAUSED;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_resume(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer resume failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    if (s_timer.state != FRIDGE_KITCHEN_TIMER_PAUSED || s_timer.remaining_seconds == 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    int64_t now = now_ms();
    s_timer.state = FRIDGE_KITCHEN_TIMER_RUNNING;
    s_timer.started_at_ms = now;
    s_timer.target_at_ms = now + ((int64_t)s_timer.remaining_seconds * 1000);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_cancel(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer cancel failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    memset(&s_timer, 0, sizeof(s_timer));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_dismiss(void)
{
    return fridge_kitchen_tools_timer_cancel();
}

esp_err_t fridge_kitchen_tools_stopwatch_start(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before stopwatch start failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "stopwatch lock timeout");
    int64_t now = now_ms();
    if (s_stopwatch.state != FRIDGE_KITCHEN_STOPWATCH_RUNNING) {
        s_stopwatch.started_at_ms = now;
        s_stopwatch.state = FRIDGE_KITCHEN_STOPWATCH_RUNNING;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_stopwatch_pause(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before stopwatch pause failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "stopwatch lock timeout");
    if (s_stopwatch.state != FRIDGE_KITCHEN_STOPWATCH_RUNNING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_stopwatch.elapsed_seconds = stopwatch_elapsed_locked(now_ms());
    s_stopwatch.state = FRIDGE_KITCHEN_STOPWATCH_PAUSED;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_stopwatch_reset(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before stopwatch reset failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "stopwatch lock timeout");
    memset(&s_stopwatch, 0, sizeof(s_stopwatch));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static uint8_t next_alarm_id_locked(void)
{
    for (uint8_t id = 1; id < 250; id++) {
        bool used = false;
        for (size_t i = 0; i < s_alarm_count; i++) {
            if (s_alarms[i].id == id) {
                used = true;
                break;
            }
        }
        if (!used) {
            return id;
        }
    }
    return 0;
}

esp_err_t fridge_kitchen_tools_alarm_set(uint8_t hour, uint8_t minute, const char *label, uint8_t *out_id)
{
    ESP_RETURN_ON_FALSE(hour <= 23 && minute <= 59, ESP_ERR_INVALID_ARG, TAG, "alarm time invalid");
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before alarm set failed");
    ESP_RETURN_ON_FALSE(time_ready(), ESP_ERR_INVALID_STATE, TAG, "wall clock is not ready");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "alarm lock timeout");
    if (s_alarm_count >= FRIDGE_KITCHEN_TOOL_MAX_ALARMS) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    uint8_t id = next_alarm_id_locked();
    if (id == 0) {
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    fridge_kitchen_alarm_t *alarm = &s_alarms[s_alarm_count++];
    memset(alarm, 0, sizeof(*alarm));
    alarm->id = id;
    alarm->enabled = true;
    alarm->hour = hour;
    alarm->minute = minute;
    copy_label(alarm->label, sizeof(alarm->label), label, "厨房提醒");
    esp_err_t err = save_alarms_locked();
    if (err != ESP_OK) {
        s_alarm_count--;
        memset(alarm, 0, sizeof(*alarm));
    }
    if (err == ESP_OK && out_id) {
        *out_id = id;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t fridge_kitchen_tools_alarm_cancel(uint8_t id)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before alarm cancel failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "alarm lock timeout");
    for (size_t i = 0; i < s_alarm_count; i++) {
        if (s_alarms[i].id == id) {
            fridge_kitchen_alarm_t backup[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
            size_t backup_count = s_alarm_count;
            memcpy(backup, s_alarms, sizeof(backup));
            if (i + 1 < s_alarm_count) {
                memmove(&s_alarms[i], &s_alarms[i + 1], (s_alarm_count - i - 1) * sizeof(s_alarms[0]));
            }
            s_alarm_count--;
            esp_err_t err = save_alarms_locked();
            if (err != ESP_OK) {
                memcpy(s_alarms, backup, sizeof(backup));
                s_alarm_count = backup_count;
            }
            xSemaphoreGive(s_lock);
            return err;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t fridge_kitchen_tools_alarm_dismiss(uint8_t id)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before alarm dismiss failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "alarm lock timeout");
    for (size_t i = 0; i < s_alarm_count; i++) {
        if (s_alarms[i].id == id) {
            bool old_ringing = s_alarms[i].ringing;
            s_alarms[i].ringing = false;
            esp_err_t err = save_alarms_locked();
            if (err != ESP_OK) {
                s_alarms[i].ringing = old_ringing;
            }
            xSemaphoreGive(s_lock);
            return err;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

static const char *json_string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int json_int(const cJSON *root, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static void ai_result(fridge_kitchen_tools_ai_result_t *out,
                      bool executed,
                      bool needs_confirmation,
                      const char *tool,
                      const char *action,
                      const char *message)
{
    if (!out) {
        return;
    }
    out->executed = executed;
    out->needs_confirmation = needs_confirmation;
    strlcpy(out->tool, tool ? tool : "", sizeof(out->tool));
    strlcpy(out->action, action ? action : "", sizeof(out->action));
    strlcpy(out->message, message ? message : "", sizeof(out->message));
}

esp_err_t fridge_kitchen_tools_execute_ai_json(const char *json, fridge_kitchen_tools_ai_result_t *out)
{
    ESP_RETURN_ON_FALSE(json && out, ESP_ERR_INVALID_ARG, TAG, "invalid AI json args");
    memset(out, 0, sizeof(*out));
    const char *start = strchr(json, '{');
    const char *end = strrchr(json, '}');
    if (!start || !end || end <= start) {
        ai_result(out, false, true, "", "", "没有找到可执行的厨房工具指令");
        return ESP_ERR_NOT_FOUND;
    }
    size_t len = (size_t)(end - start + 1);
    char *slice = calloc(1, len + 1);
    ESP_RETURN_ON_FALSE(slice, ESP_ERR_NO_MEM, TAG, "allocate AI json slice failed");
    memcpy(slice, start, len);
    cJSON *root = cJSON_Parse(slice);
    free(slice);
    if (!root) {
        ai_result(out, false, true, "", "", "厨房工具指令不是有效 JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *tool = json_string(root, "tool");
    const char *action = json_string(root, "action");
    const char *label = json_string(root, "label");
    const cJSON *field = NULL;
    cJSON_ArrayForEach(field, root) {
        if (!json_key_allowed(field->string)) {
            cJSON_Delete(root);
            ai_result(out, false, true, "", "", "厨房工具指令包含不支持的字段");
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!tool || !action) {
        cJSON_Delete(root);
        ai_result(out, false, true, "", "", "厨房工具指令缺少 tool 或 action");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_INVALID_ARG;
    char message[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1] = {0};
    if (strcmp(tool, "timer") == 0) {
        if (strcmp(action, "start") == 0) {
            int seconds = json_int(root, "duration_seconds", 0);
            if (seconds <= 0) {
                cJSON_Delete(root);
                ai_result(out, false, true, tool, action, "定时器缺少有效时长");
                return ESP_ERR_INVALID_ARG;
            }
            err = fridge_kitchen_tools_timer_start((uint32_t)seconds, label);
            snprintf(message, sizeof(message), "已启动%s定时器，%d分钟%02d秒",
                     label && label[0] ? label : "厨房",
                     seconds / 60,
                     seconds % 60);
        } else if (strcmp(action, "pause") == 0) {
            err = fridge_kitchen_tools_timer_pause();
            strlcpy(message, "定时器已暂停", sizeof(message));
        } else if (strcmp(action, "resume") == 0) {
            err = fridge_kitchen_tools_timer_resume();
            strlcpy(message, "定时器已继续", sizeof(message));
        } else if (strcmp(action, "cancel") == 0 || strcmp(action, "dismiss") == 0) {
            err = fridge_kitchen_tools_timer_cancel();
            strlcpy(message, "定时器已取消", sizeof(message));
        }
    } else if (strcmp(tool, "stopwatch") == 0) {
        if (strcmp(action, "start") == 0 || strcmp(action, "resume") == 0) {
            err = fridge_kitchen_tools_stopwatch_start();
            strlcpy(message, "秒表已开始", sizeof(message));
        } else if (strcmp(action, "pause") == 0) {
            err = fridge_kitchen_tools_stopwatch_pause();
            strlcpy(message, "秒表已暂停", sizeof(message));
        } else if (strcmp(action, "reset") == 0 || strcmp(action, "cancel") == 0) {
            err = fridge_kitchen_tools_stopwatch_reset();
            strlcpy(message, "秒表已重置", sizeof(message));
        }
    } else if (strcmp(tool, "alarm") == 0) {
        if (strcmp(action, "set") == 0) {
            int hour = json_int(root, "hour", -1);
            int minute = json_int(root, "minute", -1);
            if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
                cJSON_Delete(root);
                ai_result(out, false, true, tool, action, "闹钟缺少有效时间");
                return ESP_ERR_INVALID_ARG;
            }
            uint8_t id = 0;
            err = fridge_kitchen_tools_alarm_set((uint8_t)hour, (uint8_t)minute, label, &id);
            snprintf(message, sizeof(message), "已设置%02d:%02d的%s闹钟", hour, minute, label && label[0] ? label : "厨房提醒");
        } else if (strcmp(action, "cancel") == 0 || strcmp(action, "dismiss") == 0) {
            int id = json_int(root, "id", -1);
            if (id < 0) {
                cJSON_Delete(root);
                ai_result(out, false, true, tool, action, "请确认要取消或停止哪个闹钟");
                return ESP_OK;
            }
            err = strcmp(action, "dismiss") == 0 ? fridge_kitchen_tools_alarm_dismiss((uint8_t)id)
                                                  : fridge_kitchen_tools_alarm_cancel((uint8_t)id);
            strlcpy(message, strcmp(action, "dismiss") == 0 ? "闹钟已停止提醒" : "闹钟已取消", sizeof(message));
        }
    } else {
        strlcpy(message, "不支持这个厨房工具", sizeof(message));
    }

    cJSON_Delete(root);
    if (err != ESP_OK) {
        if (message[0] == '\0') {
            strlcpy(message, kitchen_err_text(err, tool, action), sizeof(message));
        }
        ai_result(out, false, true, tool, action, message);
        return err;
    }
    if (message[0] == '\0') {
        ai_result(out, false, true, tool, action, "不支持这个厨房工具动作");
        return ESP_ERR_INVALID_ARG;
    }
    ai_result(out, true, false, tool, action, message);
    return ESP_OK;
}

const char *fridge_kitchen_timer_state_text(fridge_kitchen_timer_state_t state)
{
    switch (state) {
    case FRIDGE_KITCHEN_TIMER_RUNNING:
        return "running";
    case FRIDGE_KITCHEN_TIMER_PAUSED:
        return "paused";
    case FRIDGE_KITCHEN_TIMER_RINGING:
        return "ringing";
    case FRIDGE_KITCHEN_TIMER_IDLE:
    default:
        return "idle";
    }
}

const char *fridge_kitchen_stopwatch_state_text(fridge_kitchen_stopwatch_state_t state)
{
    switch (state) {
    case FRIDGE_KITCHEN_STOPWATCH_RUNNING:
        return "running";
    case FRIDGE_KITCHEN_STOPWATCH_PAUSED:
        return "paused";
    case FRIDGE_KITCHEN_STOPWATCH_IDLE:
    default:
        return "idle";
    }
}
