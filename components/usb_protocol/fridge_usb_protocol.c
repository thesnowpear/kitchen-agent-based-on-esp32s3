// 冰箱小精灵 USB JSON Lines 协议组件。
// 负责从串口读取 Web 面板请求，并输出单行 JSON 响应；普通 ESP_LOG 日志仍可并行输出。
// 注意：密码字段只用于调用网络组件，不会在响应和日志中回显。

#include "fridge_usb_protocol.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fridge_ai_client.h"
#include "fridge_ai_context.h"
#include "fridge_ai_actions.h"
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "fridge_camera.h"
#include "fridge_diagnostics.h"
#include "fridge_kitchen_tools.h"
#include "fridge_mqtt_protocol.h"
#include "fridge_network.h"
#include "fridge_radar.h"
#include "fridge_sensors.h"
#include "fridge_speaker.h"
#include "fridge_state_machine.h"
#include "fridge_storage.h"
#include "fridge_touch.h"
#include "fridge_voice_session.h"
#include "fridge_wake_word.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define USB_LINE_BUFFER_SIZE 16384
#define USB_VALUE_BUFFER_SIZE 128
#define USB_SCAN_MAX_AP 50
#define USB_PROTOCOL_TASK_STACK 12288
#define USB_AI_WORKER_TASK_STACK 32768
#define USB_CAMERA_WORKER_TASK_STACK 32768

static const char *TAG = "usb_protocol";
static char s_usb_line_buffer[USB_LINE_BUFFER_SIZE];

typedef struct {
    const char *message;
    fridge_ai_chat_result_t *result;
    esp_err_t err;
    SemaphoreHandle_t done;
} usb_ai_chat_job_t;

typedef struct {
    fridge_ai_assistant_request_t *request;
    fridge_ai_assistant_result_t *result;
    esp_err_t err;
    SemaphoreHandle_t done;
} usb_ai_assistant_job_t;

typedef struct {
    fridge_voice_session_result_t *result;
    esp_err_t err;
    SemaphoreHandle_t done;
} usb_voice_chat_job_t;

typedef struct {
    fridge_ai_image_result_t *result;
    esp_err_t err;
    SemaphoreHandle_t done;
} usb_camera_analyze_job_t;

static void *usb_large_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static BaseType_t usb_create_worker_task(TaskFunction_t task,
                                         const char *name,
                                         uint32_t stack_bytes,
                                         void *arg,
                                         UBaseType_t priority,
                                         TaskHandle_t *handle)
{
    // AI、语音和图片识别 worker 栈较大，优先放 PSRAM，避免完整 UI 运行时内部 SRAM 不足导致任务创建失败。
    BaseType_t ok = xTaskCreateWithCaps(task,
                                        name,
                                        stack_bytes,
                                        arg,
                                        priority,
                                        handle,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) {
        return ok;
    }
    ESP_LOGW(TAG,
             "%s PSRAM stack create failed, retry internal stack=%lu, free_heap=%u KB, largest_internal=%u bytes, free_psram=%u KB",
             name,
             (unsigned long)stack_bytes,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return xTaskCreate(task, name, stack_bytes, arg, priority, handle);
}

static char *base64_encode_alloc(const uint8_t *data, size_t len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!data || len == 0) {
        return NULL;
    }

    size_t out_len = ((len + 2) / 3) * 4;
    char *out = usb_large_alloc(out_len + 1);
    if (!out) {
        return NULL;
    }

    size_t i = 0;
    size_t j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[j++] = table[(triple >> 18) & 0x3F];
        out[j++] = table[(triple >> 12) & 0x3F];
        out[j++] = table[(triple >> 6) & 0x3F];
        out[j++] = table[triple & 0x3F];
    }

    size_t mod = len % 3;
    if (mod) {
        out[out_len - 1] = '=';
        if (mod == 1) {
            out[out_len - 2] = '=';
        }
    }
    out[out_len] = '\0';
    return out;
}

// 生成 16 kHz / mono / signed 16-bit PCM 的 WAV 头。
// Web 面板用它直接播放开发板麦克风采到的原始声音，便于判断噪声、增益和接线问题。
static void write_wav_header(uint8_t *header, size_t pcm_bytes)
{
    const uint32_t sample_rate = FRIDGE_AUDIO_SAMPLE_RATE;
    const uint16_t channels = 1;
    const uint16_t bits_per_sample = 16;
    const uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
    const uint16_t block_align = channels * bits_per_sample / 8;
    const uint32_t riff_size = (uint32_t)(36 + pcm_bytes);
    const uint32_t data_size = (uint32_t)pcm_bytes;

    memcpy(header + 0, "RIFF", 4);
    header[4] = (uint8_t)(riff_size & 0xFF);
    header[5] = (uint8_t)((riff_size >> 8) & 0xFF);
    header[6] = (uint8_t)((riff_size >> 16) & 0xFF);
    header[7] = (uint8_t)((riff_size >> 24) & 0xFF);
    memcpy(header + 8, "WAVEfmt ", 8);
    header[16] = 16;
    header[17] = 0;
    header[18] = 0;
    header[19] = 0;
    header[20] = 1;
    header[21] = 0;
    header[22] = (uint8_t)(channels & 0xFF);
    header[23] = (uint8_t)((channels >> 8) & 0xFF);
    header[24] = (uint8_t)(sample_rate & 0xFF);
    header[25] = (uint8_t)((sample_rate >> 8) & 0xFF);
    header[26] = (uint8_t)((sample_rate >> 16) & 0xFF);
    header[27] = (uint8_t)((sample_rate >> 24) & 0xFF);
    header[28] = (uint8_t)(byte_rate & 0xFF);
    header[29] = (uint8_t)((byte_rate >> 8) & 0xFF);
    header[30] = (uint8_t)((byte_rate >> 16) & 0xFF);
    header[31] = (uint8_t)((byte_rate >> 24) & 0xFF);
    header[32] = (uint8_t)(block_align & 0xFF);
    header[33] = (uint8_t)((block_align >> 8) & 0xFF);
    header[34] = (uint8_t)(bits_per_sample & 0xFF);
    header[35] = (uint8_t)((bits_per_sample >> 8) & 0xFF);
    memcpy(header + 36, "data", 4);
    header[40] = (uint8_t)(data_size & 0xFF);
    header[41] = (uint8_t)((data_size >> 8) & 0xFF);
    header[42] = (uint8_t)((data_size >> 16) & 0xFF);
    header[43] = (uint8_t)((data_size >> 24) & 0xFF);
}

static void json_print_escaped(const char *text)
{
    putchar('"');
    for (const char *p = text ? text : ""; *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            putchar(*p);
            break;
        }
    }
    putchar('"');
}

static const char *find_json_key(const char *json, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) {
        return NULL;
    }
    pos += strlen(pattern);
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos != ':') {
        return NULL;
    }
    pos++;
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    return pos;
}

static bool json_has_key(const char *json, const char *key)
{
    return find_json_key(json, key) != NULL;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool append_utf8(char *out, size_t out_size, size_t *written, uint32_t codepoint)
{
    if (!out || !written || *written >= out_size) {
        return false;
    }

    unsigned char bytes[4];
    size_t count = 0;
    if (codepoint <= 0x7F) {
        bytes[count++] = (unsigned char)codepoint;
    } else if (codepoint <= 0x7FF) {
        bytes[count++] = (unsigned char)(0xC0 | (codepoint >> 6));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        bytes[count++] = (unsigned char)(0xE0 | (codepoint >> 12));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        bytes[count++] = (unsigned char)(0xF0 | (codepoint >> 18));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3F));
    }

    if (*written + count >= out_size) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        out[(*written)++] = (char)bytes[i];
    }
    return true;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    const char *pos = find_json_key(json, key);
    if (!pos || *pos != '"' || !out || out_size == 0) {
        return false;
    }
    pos++;

    size_t written = 0;
    while (*pos && *pos != '"') {
        char ch = *pos++;
        if (ch == '\\' && *pos) {
            char esc = *pos++;
            if (esc == 'n') {
                ch = '\n';
            } else if (esc == 'r') {
                ch = '\r';
            } else if (esc == 't') {
                ch = '\t';
            } else if (esc == 'u') {
                // Web Serial 通常直接发送 UTF-8，但 PowerShell/部分调试工具会发送 \uXXXX。
                // 这里解码为 UTF-8，避免第二轮对话 history 或 message 被解析成损坏文本。
                uint32_t codepoint = 0;
                bool valid = true;
                for (int i = 0; i < 4; i++) {
                    int value = hex_value(*pos++);
                    if (value < 0) {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 4) | (uint32_t)value;
                }
                if (valid) {
                    append_utf8(out, out_size, &written, codepoint);
                    continue;
                }
                ch = '?';
            } else {
                ch = esc;
            }
        }
        if (written + 1 < out_size) {
            out[written++] = ch;
        }
    }
    out[written] = '\0';
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool default_value)
{
    const char *pos = find_json_key(json, key);
    if (!pos) {
        return default_value;
    }
    if (strncmp(pos, "true", 4) == 0) {
        return true;
    }
    if (strncmp(pos, "false", 5) == 0) {
        return false;
    }
    return default_value;
}

static uint32_t json_get_u32(const char *json, const char *key, uint32_t default_value)
{
    const char *pos = find_json_key(json, key);
    if (!pos || !isdigit((unsigned char)*pos)) {
        return default_value;
    }
    unsigned long value = strtoul(pos, NULL, 10);
    return value > UINT32_MAX ? default_value : (uint32_t)value;
}

static const char *skip_json_string(const char *pos)
{
    if (!pos || *pos != '"') {
        return NULL;
    }
    pos++;
    while (*pos) {
        if (*pos == '\\' && pos[1]) {
            pos += 2;
            continue;
        }
        if (*pos == '"') {
            return pos + 1;
        }
        pos++;
    }
    return NULL;
}

static const char *skip_json_value(const char *pos)
{
    if (!pos) {
        return NULL;
    }
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos == '"') {
        return skip_json_string(pos);
    }
    if (*pos == '{' || *pos == '[') {
        char open = *pos;
        char close = open == '{' ? '}' : ']';
        int depth = 0;
        while (*pos) {
            if (*pos == '"') {
                pos = skip_json_string(pos);
                if (!pos) {
                    return NULL;
                }
                continue;
            }
            if (*pos == open) {
                depth++;
            } else if (*pos == close) {
                depth--;
                if (depth == 0) {
                    return pos + 1;
                }
            }
            pos++;
        }
        return NULL;
    }
    while (*pos && *pos != ',' && *pos != '}' && *pos != ']') {
        pos++;
    }
    return pos;
}

static bool json_get_object_raw(const char *json, const char *key, char *out, size_t out_size)
{
    const char *pos = find_json_key(json, key);
    if (!pos || *pos != '{' || !out || out_size == 0) {
        return false;
    }
    const char *end = skip_json_value(pos);
    if (!end || end <= pos || (size_t)(end - pos) >= out_size) {
        return false;
    }
    memcpy(out, pos, (size_t)(end - pos));
    out[end - pos] = '\0';
    return true;
}

static size_t convert_storage_history_to_ai_history(const fridge_storage_chat_history_t *storage_history,
                                                    fridge_ai_chat_history_item_t *history,
                                                    size_t max_history)
{
    if (!storage_history || !history || max_history == 0) {
        return 0;
    }

    size_t source_count = storage_history->count;
    if (source_count > FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
        source_count = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
    }
    if (source_count > 0 && strcmp(storage_history->messages[source_count - 1].role, "user") == 0) {
        // 历史注入只使用已经完成的 user/assistant 轮次；最后一条 user 可能是上次异常中断遗留。
        source_count--;
    }
    if (source_count % 2 != 0) {
        source_count--;
    }
    size_t pair_limit = (max_history / 2) * 2;
    if (pair_limit == 0) {
        return 0;
    }
    size_t start = source_count > pair_limit ? (source_count - pair_limit) : 0;
    if (start % 2 != 0) {
        start++;
    }

    size_t count = 0;
    for (size_t i = start; i < source_count && count < max_history; i++) {
        const fridge_storage_chat_message_t *message = &storage_history->messages[i];
        if (message->content[0] == '\0') {
            continue;
        }
        bool expect_user = (count % 2) == 0;
        if ((expect_user && strcmp(message->role, "user") != 0) ||
            (!expect_user && strcmp(message->role, "assistant") != 0)) {
            // OpenAI-compatible 服务对 history 顺序更严格，遇到坏轮次时宁可重新截断。
            count = 0;
            continue;
        }
        strlcpy(history[count].role,
                strcmp(message->role, "assistant") == 0 ? "assistant" : "user",
                sizeof(history[count].role));
        strlcpy(history[count].content, message->content, sizeof(history[count].content));
        count++;
    }
    return (count / 2) * 2;
}

static void response_begin(const char *request_id, const char *command, bool ok)
{
    flockfile(stdout);
    fputs("{\"type\":\"response\",\"request_id\":", stdout);
    json_print_escaped(request_id);
    fputs(",\"ok\":", stdout);
    fputs(ok ? "true" : "false", stdout);
    fputs(",\"command\":", stdout);
    json_print_escaped(command);
}

static void response_end(void)
{
    fputs("}\n", stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void event_begin(const char *event)
{
    flockfile(stdout);
    fputs("{\"type\":\"event\",\"event\":", stdout);
    json_print_escaped(event);
}

static void event_end(void)
{
    fputs("}\n", stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void send_error(const char *request_id, const char *command, const char *error)
{
    response_begin(request_id, command, false);
    fputs(",\"error\":", stdout);
    json_print_escaped(error);
    fputs(",\"payload\":{}", stdout);
    response_end();
}

static void print_state_machine_config_payload(const fridge_sm_config_t *config)
{
    fputs("{", stdout);
    printf("\"nightLightThreshold\":%u,\"dayLightThreshold\":%u,"
           "\"radarTwoMeterRaw\":%u,\"radarTwoMeterGate\":%u,"
           "\"sleepEnabled\":%s,\"autoVoiceAfterClose\":%s,"
           "\"autoVoiceRecordSeconds\":%lu,\"closeStableMs\":%lu",
           (unsigned)config->night_light_threshold,
           (unsigned)config->day_light_threshold,
           (unsigned)config->radar_two_meter_raw,
           (unsigned)config->radar_two_meter_gate,
           config->sleep_enabled ? "true" : "false",
           config->auto_voice_after_close ? "true" : "false",
           (unsigned long)config->auto_voice_record_seconds,
           (unsigned long)config->close_stable_ms);
    fputs("}", stdout);
}

static void print_state_machine_payload(const fridge_sm_snapshot_t *snapshot)
{
    fputs("{\"state\":", stdout);
    json_print_escaped(fridge_state_machine_state_to_string(snapshot->state));
    fputs(",\"doorState\":", stdout);
    json_print_escaped(fridge_state_machine_door_to_string(snapshot->door_state));
    printf(",\"offline\":%s,\"isNight\":%s,\"radarSoftwarePaused\":%s,"
           "\"radarPresenceReliable\":%s,\"radarWithin2m\":%s,\"radarWithin1m\":%s,"
           "\"radarApproaching\":%s,\"imuMotionStrength\":%.3f,"
           "\"lightValue10bit\":%u,\"lightDelta\":%d,"
           "\"radarDistanceRaw\":%u,\"radarGate\":%u,",
           snapshot->offline ? "true" : "false",
           snapshot->is_night ? "true" : "false",
           snapshot->radar_software_paused ? "true" : "false",
           snapshot->radar_presence_reliable ? "true" : "false",
           snapshot->radar_within_2m ? "true" : "false",
           snapshot->radar_within_1m ? "true" : "false",
           snapshot->radar_approaching ? "true" : "false",
           (double)snapshot->imu_motion_strength,
           (unsigned)snapshot->light_value_10bit,
           (int)snapshot->light_delta,
           (unsigned)snapshot->radar_distance_raw,
           (unsigned)snapshot->radar_gate);
    fputs("\"lastReason\":", stdout);
    json_print_escaped(snapshot->last_reason);
    fputs(",\"autoVoiceState\":", stdout);
    json_print_escaped(fridge_state_machine_auto_voice_to_string(snapshot->auto_voice_state));
    fputs(",\"autoVoiceError\":", stdout);
    json_print_escaped(snapshot->auto_voice_error);
    printf(",\"updatedAtMs\":%lld,\"stateSinceMs\":%lld}",
           (long long)snapshot->updated_at_ms,
           (long long)snapshot->state_since_ms);
}

static void print_network_payload(void)
{
    fridge_network_status_t status = {0};
    fridge_network_get_status(&status);
    fridge_mqtt_status_t mqtt_status = {0};
    fridge_mqtt_get_status(&mqtt_status);

    fputs("{\"ssid\":", stdout);
    json_print_escaped(status.ssid);
    fputs(",\"wifiPassword\":\"\",\"mqttHost\":", stdout);
    json_print_escaped(mqtt_status.broker_uri);
    fputs(",\"apiBaseUrl\":\"\",\"ntpServer\":", stdout);
    json_print_escaped(status.ntp_server);
    fputs(",\"saveAiKey\":false,\"connected\":", stdout);
    fputs(status.connected ? "true" : "false", stdout);
    fputs(",\"connecting\":", stdout);
    fputs(status.connecting ? "true" : "false", stdout);
    fputs(",\"saved\":", stdout);
    fputs(status.saved ? "true" : "false", stdout);
    fputs(",\"internet\":", stdout);
    fputs(status.internet_ready ? "true" : "false", stdout);
    fputs(",\"status\":", stdout);
    json_print_escaped(status.connected ? (status.internet_ready ? "online" : "wifi_connected") : (status.connecting ? "connecting" : "offline"));
    fputs(",\"ip\":", stdout);
    json_print_escaped(status.ip);
    printf(",\"rssi\":%d", status.rssi);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(status.last_error);
    fputs("}", stdout);
}

static void handle_get_network(const char *request_id, const char *command)
{
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_network_payload();
    response_end();
}

static void handle_scan_wifi(const char *request_id, const char *command)
{
    fridge_wifi_ap_t aps[USB_SCAN_MAX_AP] = {0};
    size_t count = 0;
    esp_err_t err = fridge_network_scan(aps, USB_SCAN_MAX_AP, &count);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":[", stdout);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            putchar(',');
        }
        fputs("{\"ssid\":", stdout);
        json_print_escaped(aps[i].ssid);
        printf(",\"rssi\":%d,\"signal\":%d,\"channel\":%u,\"secured\":%s,\"band\":\"2.4G\",\"authmode\":",
               aps[i].rssi,
               aps[i].rssi <= -100 ? 0 : (aps[i].rssi >= -50 ? 100 : 2 * (aps[i].rssi + 100)),
               aps[i].channel,
               aps[i].secured ? "true" : "false");
        json_print_escaped(aps[i].authmode);
        fputs(",\"note\":\"真实扫描结果\"}", stdout);
    }
    fputs("]", stdout);
    response_end();
}

static void handle_set_network(const char *line, const char *request_id, const char *command)
{
    fridge_wifi_config_t config = {0};
    json_get_string(line, "ssid", config.ssid, sizeof(config.ssid));
    json_get_string(line, "wifiPassword", config.password, sizeof(config.password));
    bool save = json_get_bool(line, "save", true);

    if (config.ssid[0] == '\0') {
        send_error(request_id, command, "ssid is required");
        return;
    }

    esp_err_t err = fridge_network_connect(&config, save);
    if (err != ESP_OK) {
        fridge_network_status_t status = {0};
        fridge_network_get_status(&status);
        send_error(request_id, command, status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_network_payload();
    response_end();
}

static void print_mqtt_config_payload(const fridge_mqtt_status_t *status)
{
    fputs("{\"brokerUri\":", stdout);
    json_print_escaped(status->broker_uri);
    fputs(",\"homeId\":", stdout);
    json_print_escaped(status->home_id);
    fputs(",\"deviceId\":", stdout);
    json_print_escaped(status->device_id);
    fputs(",\"username\":", stdout);
    json_print_escaped(status->username);
    fputs(",\"hasPassword\":", stdout);
    fputs(status->has_password ? "true" : "false", stdout);
    fputs(",\"enabled\":", stdout);
    fputs(status->enabled ? "true" : "false", stdout);
    fputs(",\"configured\":", stdout);
    fputs(status->configured ? "true" : "false", stdout);
    fputs(",\"connected\":", stdout);
    fputs(status->connected ? "true" : "false", stdout);
    printf(",\"reconnectCount\":%lu,\"publishedCount\":%lu,\"receivedCount\":%lu,\"lastError\":%d",
           (unsigned long)status->reconnect_count,
           (unsigned long)status->published_count,
           (unsigned long)status->received_count,
           status->last_error);
    fputs(",\"statusText\":", stdout);
    json_print_escaped(status->status_text);
    fputs("}", stdout);
}

static void handle_get_mqtt_config(const char *request_id, const char *command)
{
    fridge_mqtt_status_t status = {0};
    esp_err_t err = fridge_mqtt_get_status(&status);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_mqtt_config_payload(&status);
    response_end();
}

static void handle_set_mqtt_config(const char *line, const char *request_id, const char *command)
{
    fridge_mqtt_config_t config = {0};
    fridge_mqtt_get_config(&config);
    json_get_string(line, "brokerUri", config.broker_uri, sizeof(config.broker_uri));
    json_get_string(line, "mqttHost", config.broker_uri, sizeof(config.broker_uri));
    json_get_string(line, "homeId", config.home_id, sizeof(config.home_id));
    json_get_string(line, "deviceId", config.device_id, sizeof(config.device_id));
    json_get_string(line, "username", config.username, sizeof(config.username));
    bool update_password = json_has_key(line, "password") || json_has_key(line, "token");
    if (json_has_key(line, "password")) {
        json_get_string(line, "password", config.password, sizeof(config.password));
    } else if (json_has_key(line, "token")) {
        json_get_string(line, "token", config.password, sizeof(config.password));
    }
    config.enabled = json_get_bool(line, "enabled", true);
    config.keepalive_seconds = (uint16_t)json_get_u32(line, "keepaliveSeconds", FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS);

    esp_err_t err = fridge_mqtt_set_config(&config, update_password);
    if (err == ESP_OK && config.enabled) {
        esp_err_t start_err = fridge_mqtt_start();
        if (start_err != ESP_OK && start_err != ESP_ERR_INVALID_STATE) {
            err = start_err;
        }
    }
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    handle_get_mqtt_config(request_id, command);
}

static void handle_clear_mqtt_secret(const char *request_id, const char *command)
{
    esp_err_t err = fridge_mqtt_clear_secret();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_mqtt_config(request_id, command);
}

static void handle_mqtt_publish_state(const char *request_id, const char *command)
{
    esp_err_t err = fridge_mqtt_start();
    if (err == ESP_OK) {
        err = fridge_mqtt_publish_state(true);
    }
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_mqtt_config(request_id, command);
}

static void print_ai_config_payload(const fridge_ai_config_view_t *config)
{
    printf("{\"profileId\":%u,\"profileName\":", (unsigned)config->profile_id);
    json_print_escaped(config->profile_name);
    fputs(",\"apiBaseUrl\":", stdout);
    json_print_escaped(config->api_base_url);
    fputs(",\"model\":", stdout);
    json_print_escaped(config->model);
    fputs(",\"systemPrompt\":", stdout);
    json_print_escaped(config->system_prompt);
    printf(",\"timeoutMs\":%lu", (unsigned long)config->timeout_ms);
    fputs(",\"hasApiKey\":", stdout);
    fputs(config->has_api_key ? "true" : "false", stdout);
    fputs(",\"apiKeyPreview\":", stdout);
    json_print_escaped(config->api_key_preview);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(config->last_error);
    fputs(",\"ready\":", stdout);
    fputs(config->ready ? "true" : "false", stdout);
    fputs("}", stdout);
}

static void print_asr_config_payload(const fridge_asr_config_view_t *config)
{
    fputs("{\"apiBaseUrl\":", stdout);
    json_print_escaped(config->api_base_url);
    fputs(",\"model\":", stdout);
    json_print_escaped(config->model);
    printf(",\"timeoutMs\":%lu", (unsigned long)config->timeout_ms);
    fputs(",\"hasApiKey\":", stdout);
    fputs(config->has_api_key ? "true" : "false", stdout);
    fputs(",\"apiKeyPreview\":", stdout);
    json_print_escaped(config->api_key_preview);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(config->last_error);
    fputs(",\"ready\":", stdout);
    fputs(config->ready ? "true" : "false", stdout);
    fputs("}", stdout);
}

static void print_tts_config_payload(const fridge_tts_config_view_t *config)
{
    fputs("{\"apiBaseUrl\":", stdout);
    json_print_escaped(config->api_base_url);
    fputs(",\"model\":", stdout);
    json_print_escaped(config->model);
    fputs(",\"voice\":", stdout);
    json_print_escaped(config->voice);
    printf(",\"timeoutMs\":%lu", (unsigned long)config->timeout_ms);
    fputs(",\"hasApiKey\":", stdout);
    fputs(config->has_api_key ? "true" : "false", stdout);
    fputs(",\"apiKeyPreview\":", stdout);
    json_print_escaped(config->api_key_preview);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(config->last_error);
    fputs(",\"ready\":", stdout);
    fputs(config->ready ? "true" : "false", stdout);
    fputs("}", stdout);
}

static void print_ai_profiles_payload(const fridge_ai_profile_list_t *profiles)
{
    printf("{\"activeProfileId\":%u,\"profiles\":[", (unsigned)profiles->active_profile_id);
    for (size_t i = 0; i < profiles->count; i++) {
        if (i > 0) {
            putchar(',');
        }
        print_ai_config_payload(&profiles->profiles[i]);
    }
    fputs("]}", stdout);
}

static void build_ai_task_request_from_line(const char *line, fridge_ai_task_request_t *request)
{
    memset(request, 0, sizeof(*request));
    // 项目 AI 任务默认注入轻量上下文；Web 调试可以逐项关闭，避免把上下文注入做成“大包全塞”。
    strlcpy(request->task_type, "chat_assist", sizeof(request->task_type));
    json_get_string(line, "taskType", request->task_type, sizeof(request->task_type));
    if (request->task_type[0] == '\0') {
        strlcpy(request->task_type, "chat_assist", sizeof(request->task_type));
    }
    json_get_string(line, "userText", request->user_text, sizeof(request->user_text));
    request->include_inventory = json_get_bool(line, "includeInventory", true);
    request->include_memory = json_get_bool(line, "includeMemory", true);
    request->include_reminders = json_get_bool(line, "includeReminders", true);
    request->include_preferences = json_get_bool(line, "includePreferences", true);
}

static void handle_get_ai_context_preview(const char *line, const char *request_id, const char *command)
{
    fridge_ai_task_request_t request = {0};
    build_ai_task_request_from_line(line, &request);

    fridge_ai_context_preview_t *preview = calloc(1, sizeof(*preview));
    if (!preview) {
        send_error(request_id, command, "AI context allocation failed");
        return;
    }

    esp_err_t err = fridge_ai_context_build_preview(&request, preview);
    if (err != ESP_OK) {
        free(preview);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"taskType\":", stdout);
    json_print_escaped(preview->task_type);
    printf(",\"localSnapshotVersion\":%lu", (unsigned long)preview->local_snapshot_version);
    fputs(",\"needsConfirmation\":", stdout);
    fputs(preview->needs_confirmation ? "true" : "false", stdout);
    fputs(",\"context\":", stdout);
    fputs(preview->preview_json, stdout);
    fputs("}", stdout);
    response_end();
    free(preview);
}

static void handle_test_ai_task(const char *line, const char *request_id, const char *command)
{
    fridge_ai_task_request_t request = {0};
    build_ai_task_request_from_line(line, &request);

    fridge_ai_task_result_t *result = calloc(1, sizeof(*result));
    if (!result) {
        send_error(request_id, command, "AI task allocation failed");
        return;
    }

    esp_err_t err = fridge_ai_context_test_task(&request, result);
    if (err != ESP_OK) {
        free(result);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"taskType\":", stdout);
    json_print_escaped(result->task_type);
    printf(",\"confidence\":%u", (unsigned)result->confidence_percent);
    fputs(",\"needsConfirmation\":", stdout);
    fputs(result->needs_confirmation ? "true" : "false", stdout);
    fputs(",\"safetyNote\":", stdout);
    json_print_escaped(result->safety_note);
    fputs(",\"result\":", stdout);
    fputs(result->result_json, stdout);
    fputs("}", stdout);
    response_end();
    free(result);
}

static void print_kitchen_tools_payload(const fridge_kitchen_tools_snapshot_t *snap)
{
    fputs("{\"timeReady\":", stdout);
    fputs(snap->time_ready ? "true" : "false", stdout);
    fputs(",\"timer\":{\"state\":", stdout);
    json_print_escaped(fridge_kitchen_timer_state_text(snap->timer.state));
    printf(",\"durationSeconds\":%lu,\"remainingSeconds\":%lu,\"label\":",
           (unsigned long)snap->timer.duration_seconds,
           (unsigned long)snap->timer.remaining_seconds);
    json_print_escaped(snap->timer.label);
    fputs("},\"stopwatch\":{\"state\":", stdout);
    json_print_escaped(fridge_kitchen_stopwatch_state_text(snap->stopwatch.state));
    printf(",\"elapsedSeconds\":%lu}", (unsigned long)snap->stopwatch.elapsed_seconds);
    fputs(",\"alarms\":[", stdout);
    for (size_t i = 0; i < snap->alarm_count; i++) {
        const fridge_kitchen_alarm_t *alarm = &snap->alarms[i];
        if (i > 0) {
            putchar(',');
        }
        printf("{\"id\":%u,\"enabled\":%s,\"ringing\":%s,\"hour\":%u,\"minute\":%u,\"label\":",
               (unsigned)alarm->id,
               alarm->enabled ? "true" : "false",
               alarm->ringing ? "true" : "false",
               (unsigned)alarm->hour,
               (unsigned)alarm->minute);
        json_print_escaped(alarm->label);
        fputs("}", stdout);
    }
    fputs("],\"lastAlert\":", stdout);
    json_print_escaped(snap->last_alert);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(snap->last_error);
    fputs("}", stdout);
}

static void handle_get_kitchen_tools(const char *request_id, const char *command)
{
    fridge_kitchen_tools_snapshot_t snap = {0};
    esp_err_t err = fridge_kitchen_tools_get_snapshot(&snap);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_kitchen_tools_payload(&snap);
    response_end();
}

static void handle_timer_start(const char *line, const char *request_id, const char *command)
{
    char payload[256] = {0};
    const char *src = line;
    if (json_get_object_raw(line, "payload", payload, sizeof(payload))) {
        src = payload;
    }
    uint32_t seconds = json_get_u32(src, "durationSeconds", json_get_u32(src, "duration_seconds", 0));
    char label[FRIDGE_KITCHEN_TOOL_MAX_LABEL_LEN + 1] = {0};
    json_get_string(src, "label", label, sizeof(label));
    esp_err_t err = fridge_kitchen_tools_timer_start(seconds, label);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_kitchen_tools(request_id, command);
}

static void handle_kitchen_simple(const char *request_id, const char *command, esp_err_t (*fn)(void))
{
    esp_err_t err = fn();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_kitchen_tools(request_id, command);
}

static void handle_alarm_set(const char *line, const char *request_id, const char *command)
{
    char payload[256] = {0};
    const char *src = line;
    if (json_get_object_raw(line, "payload", payload, sizeof(payload))) {
        src = payload;
    }
    uint32_t hour = json_get_u32(src, "hour", UINT32_MAX);
    uint32_t minute = json_get_u32(src, "minute", UINT32_MAX);
    char label[FRIDGE_KITCHEN_TOOL_MAX_LABEL_LEN + 1] = {0};
    json_get_string(src, "label", label, sizeof(label));
    if (hour > 23 || minute > 59) {
        send_error(request_id, command, esp_err_to_name(ESP_ERR_INVALID_ARG));
        return;
    }
    uint8_t id = 0;
    esp_err_t err = fridge_kitchen_tools_alarm_set((uint8_t)hour, (uint8_t)minute, label, &id);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_kitchen_tools(request_id, command);
}

static void handle_alarm_id_action(const char *line, const char *request_id, const char *command, esp_err_t (*fn)(uint8_t))
{
    char payload[128] = {0};
    const char *src = line;
    if (json_get_object_raw(line, "payload", payload, sizeof(payload))) {
        src = payload;
    }
    uint32_t id = json_get_u32(src, "id", 0);
    esp_err_t err = fn((uint8_t)id);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_kitchen_tools(request_id, command);
}

static void handle_get_memory_summary(const char *request_id, const char *command)
{
    char *memory = calloc(1, FRIDGE_STORAGE_MAX_MEMORY_LEN + 1);
    if (!memory) {
        send_error(request_id, command, "memory allocation failed");
        return;
    }

    esp_err_t err = fridge_storage_get_memory_summary(memory, FRIDGE_STORAGE_MAX_MEMORY_LEN + 1);
    if (err != ESP_OK) {
        free(memory);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    fputs(memory, stdout);
    response_end();
    free(memory);
}

static void handle_clear_memory_summary(const char *request_id, const char *command)
{
    esp_err_t err = fridge_storage_clear_memory_summary();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_memory_summary(request_id, command);
}

static void handle_set_memory_summary(const char *line, const char *request_id, const char *command)
{
    char *memory = calloc(1, FRIDGE_STORAGE_MAX_MEMORY_LEN + 1);
    if (!memory) {
        send_error(request_id, command, "memory allocation failed");
        return;
    }

    bool found = json_get_object_raw(line, "memory", memory, FRIDGE_STORAGE_MAX_MEMORY_LEN + 1);
    if (!found) {
        found = json_get_object_raw(line, "payload", memory, FRIDGE_STORAGE_MAX_MEMORY_LEN + 1);
    }
    if (!found) {
        free(memory);
        send_error(request_id, command, "memory JSON object is required");
        return;
    }

    esp_err_t err = fridge_storage_set_memory_summary(memory);
    free(memory);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_memory_summary(request_id, command);
}

static void print_chat_history_payload(const fridge_storage_chat_history_t *history, size_t pruned_count)
{
    printf("{\"schemaVersion\":%lu,\"updatedAt\":%lu,\"ttlSeconds\":%lu,\"maxMessages\":%lu,"
           "\"timeReady\":%s,\"count\":%u,\"prunedCount\":%u,\"messages\":[",
           (unsigned long)history->schema_version,
           (unsigned long)history->updated_at,
           (unsigned long)history->ttl_seconds,
           (unsigned long)history->max_messages,
           history->time_ready ? "true" : "false",
           (unsigned)history->count,
           (unsigned)pruned_count);
    for (size_t i = 0; i < history->count && i < FRIDGE_STORAGE_MAX_CHAT_MESSAGES; i++) {
        if (i > 0) {
            putchar(',');
        }
        fputs("{\"id\":", stdout);
        json_print_escaped(history->messages[i].id);
        fputs(",\"role\":", stdout);
        json_print_escaped(history->messages[i].role);
        fputs(",\"content\":", stdout);
        json_print_escaped(history->messages[i].content);
        fputs(",\"taskType\":", stdout);
        json_print_escaped(history->messages[i].task_type);
        printf(",\"createdAt\":%" PRId64 "}", history->messages[i].created_at);
    }
    fputs("]}", stdout);
}

static void handle_get_chat_history(const char *request_id, const char *command)
{
    fridge_storage_chat_history_t *history = calloc(1, sizeof(*history));
    if (!history) {
        send_error(request_id, command, "chat history allocation failed");
        return;
    }
    size_t pruned_count = 0;
    esp_err_t err = fridge_storage_get_chat_history(history, &pruned_count);
    if (err != ESP_OK) {
        free(history);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_chat_history_payload(history, pruned_count);
    response_end();
    free(history);
}

static void handle_clear_chat_history(const char *request_id, const char *command)
{
    esp_err_t err = fridge_storage_clear_chat_history();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_chat_history(request_id, command);
}

static void handle_restore_seed_inventory(const char *request_id, const char *command)
{
    esp_err_t err = fridge_storage_restore_seed_inventory();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    // 恢复后立即尝试上报；若 MQTT 暂离线，dirty 标记会保留到下次重连。
    esp_err_t publish_err = fridge_mqtt_publish_inventory_snapshot(true);
    response_begin(request_id, command, publish_err == ESP_OK || publish_err == ESP_ERR_INVALID_STATE);
    printf(",\"payload\":{\"restored\":true,\"publishAttempt\":\"%s\"}", esp_err_to_name(publish_err));
    response_end();
}

static void handle_get_ai_config(const char *request_id, const char *command)
{
    fridge_ai_config_view_t *config = calloc(1, sizeof(*config));
    if (!config) {
        send_error(request_id, command, "AI config allocation failed");
        return;
    }

    esp_err_t err = fridge_ai_client_get_config(config);
    if (err != ESP_OK) {
        free(config);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_ai_config_payload(config);
    response_end();
    free(config);
}

static void handle_get_ai_profiles(const char *request_id, const char *command)
{
    fridge_ai_profile_list_t *profiles = calloc(1, sizeof(*profiles));
    if (!profiles) {
        send_error(request_id, command, "AI profiles allocation failed");
        return;
    }

    esp_err_t err = fridge_ai_client_get_profiles(profiles);
    if (err != ESP_OK) {
        free(profiles);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_ai_profiles_payload(profiles);
    response_end();
    free(profiles);
}

static void handle_set_ai_config(const char *line, const char *request_id, const char *command)
{
    fridge_ai_config_view_t *current = calloc(1, sizeof(*current));
    fridge_ai_config_update_t *update = calloc(1, sizeof(*update));
    if (!current || !update) {
        free(current);
        free(update);
        send_error(request_id, command, "AI config allocation failed");
        return;
    }

    esp_err_t err = fridge_ai_client_get_config(current);
    if (err != ESP_OK) {
        free(current);
        free(update);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    update->profile_id = (uint8_t)json_get_u32(line, "profileId", current->profile_id);
    strlcpy(update->profile_name, current->profile_name, sizeof(update->profile_name));
    json_get_string(line, "profileName", update->profile_name, sizeof(update->profile_name));
    strlcpy(update->api_base_url, current->api_base_url, sizeof(update->api_base_url));
    strlcpy(update->model, current->model[0] ? current->model : FRIDGE_AI_DEFAULT_MODEL, sizeof(update->model));
    strlcpy(update->system_prompt, current->system_prompt, sizeof(update->system_prompt));
    update->timeout_ms = current->timeout_ms ? current->timeout_ms : FRIDGE_AI_DEFAULT_TIMEOUT_MS;

    json_get_string(line, "apiBaseUrl", update->api_base_url, sizeof(update->api_base_url));
    json_get_string(line, "model", update->model, sizeof(update->model));
    json_get_string(line, "systemPrompt", update->system_prompt, sizeof(update->system_prompt));
    update->timeout_ms = json_get_u32(line, "timeoutMs", update->timeout_ms);

    if (json_has_key(line, "apiKey")) {
        json_get_string(line, "apiKey", update->api_key, sizeof(update->api_key));
        update->update_api_key = update->api_key[0] != '\0';
    }

    err = fridge_ai_client_set_config(update);
    free(current);
    free(update);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    handle_get_ai_config(request_id, command);
}

static void handle_clear_ai_key(const char *request_id, const char *command)
{
    esp_err_t err = fridge_ai_client_clear_key();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_ai_config(request_id, command);
}

static void handle_get_asr_config(const char *request_id, const char *command)
{
    fridge_asr_config_view_t config = {0};
    esp_err_t err = fridge_asr_get_config(&config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_asr_config_payload(&config);
    response_end();
}

static void handle_set_asr_config(const char *line, const char *request_id, const char *command)
{
    fridge_asr_config_view_t current = {0};
    fridge_asr_config_update_t update = {0};
    esp_err_t err = fridge_asr_get_config(&current);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    strlcpy(update.api_base_url, current.api_base_url[0] ? current.api_base_url : FRIDGE_ASR_DEFAULT_URL, sizeof(update.api_base_url));
    strlcpy(update.model, current.model[0] ? current.model : FRIDGE_ASR_DEFAULT_MODEL, sizeof(update.model));
    update.timeout_ms = current.timeout_ms ? current.timeout_ms : FRIDGE_ASR_DEFAULT_TIMEOUT_MS;
    json_get_string(line, "apiBaseUrl", update.api_base_url, sizeof(update.api_base_url));
    json_get_string(line, "model", update.model, sizeof(update.model));
    update.timeout_ms = json_get_u32(line, "timeoutMs", update.timeout_ms);
    if (json_has_key(line, "apiKey")) {
        json_get_string(line, "apiKey", update.api_key, sizeof(update.api_key));
        update.update_api_key = update.api_key[0] != '\0';
    }

    err = fridge_asr_set_config(&update);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_asr_config(request_id, command);
}

static void handle_clear_asr_key(const char *request_id, const char *command)
{
    esp_err_t err = fridge_asr_clear_key();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_asr_config(request_id, command);
}

static void handle_get_tts_config(const char *request_id, const char *command)
{
    fridge_tts_config_view_t config = {0};
    esp_err_t err = fridge_tts_get_config(&config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_tts_config_payload(&config);
    response_end();
}

static void handle_set_tts_config(const char *line, const char *request_id, const char *command)
{
    fridge_tts_config_view_t current = {0};
    fridge_tts_config_update_t update = {0};
    esp_err_t err = fridge_tts_get_config(&current);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    strlcpy(update.api_base_url, current.api_base_url[0] ? current.api_base_url : FRIDGE_TTS_DEFAULT_URL, sizeof(update.api_base_url));
    strlcpy(update.model, current.model[0] ? current.model : FRIDGE_TTS_DEFAULT_MODEL, sizeof(update.model));
    strlcpy(update.voice, current.voice[0] ? current.voice : FRIDGE_TTS_DEFAULT_VOICE, sizeof(update.voice));
    update.timeout_ms = current.timeout_ms ? current.timeout_ms : FRIDGE_TTS_DEFAULT_TIMEOUT_MS;
    json_get_string(line, "apiBaseUrl", update.api_base_url, sizeof(update.api_base_url));
    json_get_string(line, "model", update.model, sizeof(update.model));
    json_get_string(line, "voice", update.voice, sizeof(update.voice));
    update.timeout_ms = json_get_u32(line, "timeoutMs", update.timeout_ms);
    if (json_has_key(line, "apiKey")) {
        json_get_string(line, "apiKey", update.api_key, sizeof(update.api_key));
        update.update_api_key = update.api_key[0] != '\0';
    }

    err = fridge_tts_set_config(&update);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_tts_config(request_id, command);
}

static void handle_clear_tts_key(const char *request_id, const char *command)
{
    esp_err_t err = fridge_tts_clear_key();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_tts_config(request_id, command);
}

static void handle_get_state_machine_config(const char *request_id, const char *command)
{
    fridge_sm_config_t config = {0};
    esp_err_t err = fridge_state_machine_get_config(&config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_state_machine_config_payload(&config);
    response_end();
}

static void handle_set_state_machine_config(const char *line, const char *request_id, const char *command)
{
    fridge_sm_config_t config = {0};
    esp_err_t err = fridge_state_machine_get_config(&config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    config.night_light_threshold = (uint16_t)json_get_u32(line, "nightLightThreshold", config.night_light_threshold);
    config.day_light_threshold = (uint16_t)json_get_u32(line, "dayLightThreshold", config.day_light_threshold);
    config.radar_two_meter_raw = (uint16_t)json_get_u32(line, "radarTwoMeterRaw", config.radar_two_meter_raw);
    config.radar_two_meter_gate = (uint8_t)json_get_u32(line, "radarTwoMeterGate", config.radar_two_meter_gate);
    config.sleep_enabled = json_get_bool(line, "sleepEnabled", config.sleep_enabled);
    config.auto_voice_after_close = json_get_bool(line, "autoVoiceAfterClose", config.auto_voice_after_close);
    config.auto_voice_record_seconds = json_get_u32(line, "autoVoiceRecordSeconds", config.auto_voice_record_seconds);
    config.close_stable_ms = json_get_u32(line, "closeStableMs", config.close_stable_ms);

    err = fridge_state_machine_set_config(&config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_state_machine_config(request_id, command);
}

static void handle_get_state_machine_status(const char *request_id, const char *command)
{
    fridge_sm_snapshot_t snapshot = {0};
    esp_err_t err = fridge_state_machine_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_state_machine_payload(&snapshot);
    response_end();
}

static void handle_create_ai_profile(const char *line, const char *request_id, const char *command)
{
    char profile_name[FRIDGE_AI_MAX_PROFILE_NAME_LEN + 1] = {0};
    json_get_string(line, "profileName", profile_name, sizeof(profile_name));

    fridge_ai_config_view_t config = {0};
    esp_err_t err = fridge_ai_client_create_profile(profile_name, &config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_ai_config_payload(&config);
    response_end();
}

static void handle_select_ai_profile(const char *line, const char *request_id, const char *command)
{
    uint8_t profile_id = (uint8_t)json_get_u32(line, "profileId", 0);
    fridge_ai_config_view_t config = {0};
    esp_err_t err = fridge_ai_client_select_profile(profile_id, &config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_ai_config_payload(&config);
    response_end();
}

static void handle_delete_ai_profile(const char *line, const char *request_id, const char *command)
{
    uint8_t profile_id = (uint8_t)json_get_u32(line, "profileId", 0);
    fridge_ai_config_view_t config = {0};
    esp_err_t err = fridge_ai_client_delete_profile(profile_id, &config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_ai_config_payload(&config);
    response_end();
}

static void ai_chat_worker_task(void *arg)
{
    usb_ai_chat_job_t *job = (usb_ai_chat_job_t *)arg;
    // AI HTTPS/TLS 调用会占用较深调用栈；放到一次性 worker 里，避免长期抬高 USB 协议任务的常驻栈。
    job->err = fridge_ai_client_test_chat(job->message, job->result);
    ESP_LOGI(TAG,
             "AI worker done, stack high watermark=%u words",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static void ai_assistant_worker_task(void *arg)
{
    usb_ai_assistant_job_t *job = (usb_ai_assistant_job_t *)arg;
    // 真实 AI 助手请求会携带项目上下文和 TLS 栈，必须放在 worker 中执行，避免 USB 协议任务栈被打满。
    job->err = fridge_ai_client_assistant_chat(job->request, job->result);
    ESP_LOGI(TAG,
             "AI assistant worker done, stack high watermark=%u words",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static void voice_chat_worker_task(void *arg)
{
    usb_voice_chat_job_t *job = (usb_voice_chat_job_t *)arg;
    // 完整语音链路已抽到公共组件，USB 与状态机共享同一套 ASR/AI/历史保存逻辑。
    job->err = fridge_voice_session_run_latest_recording(job->result);

    ESP_LOGI(TAG, "voice chat worker done, stack high watermark=%u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static void camera_analyze_worker_task(void *arg)
{
    usb_camera_analyze_job_t *job = (usb_camera_analyze_job_t *)arg;
    esp_err_t err = fridge_camera_capture_ai_fullres();
    fridge_camera_frame_view_t frame = {0};
    if (err == ESP_OK) {
        err = fridge_camera_get_frame(&frame);
    }
    if (err == ESP_OK) {
        fridge_ai_image_request_t request = {
            .jpeg = frame.data,
            .jpeg_len = frame.len,
            .width = frame.width,
            .height = frame.height,
        };
        strlcpy(request.task_type, "recognize_ingredients", sizeof(request.task_type));
        err = fridge_ai_client_analyze_image(&request, job->result);
    }
    if (err != ESP_OK && job->result->chat.error[0] == '\0') {
        // 清理帧缓存会重置 camera last_error；先把硬件侧失败原因转存到 AI 结果，便于 Web/串口显示。
        fridge_camera_status_t status = {0};
        fridge_camera_get_status(&status);
        strlcpy(job->result->chat.error,
                status.last_error[0] ? status.last_error : esp_err_to_name(err),
                sizeof(job->result->chat.error));
    }
    job->err = err;
    (void)fridge_camera_clear_frame();
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static void handle_test_ai_chat(const char *line, const char *request_id, const char *command)
{
    char *message = calloc(1, FRIDGE_AI_MAX_CHAT_MESSAGE_LEN + 1);
    fridge_ai_chat_result_t *result = calloc(1, sizeof(*result));
    if (!message || !result) {
        free(message);
        free(result);
        send_error(request_id, command, "AI 测试内存不足");
        return;
    }

    json_get_string(line, "message", message, FRIDGE_AI_MAX_CHAT_MESSAGE_LEN + 1);
    if (message[0] == '\0') {
        free(message);
        free(result);
        send_error(request_id, command, "message is required");
        return;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        free(message);
        free(result);
        send_error(request_id, command, "AI 测试同步对象创建失败");
        return;
    }

    usb_ai_chat_job_t job = {
        .message = message,
        .result = result,
        .err = ESP_FAIL,
        .done = done,
    };
    BaseType_t task_ok = usb_create_worker_task(ai_chat_worker_task, "ai_chat_worker", USB_AI_WORKER_TASK_STACK, &job, 4, NULL);
    if (task_ok != pdPASS) {
        vSemaphoreDelete(done);
        free(message);
        free(result);
        send_error(request_id, command, "AI worker 任务创建失败");
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    ESP_LOGI(TAG,
             "USB protocol stack high watermark=%u words after AI test",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    esp_err_t err = job.err;
    if (err != ESP_OK) {
        send_error(request_id, command, result->error[0] ? result->error : esp_err_to_name(err));
        free(message);
        free(result);
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"reply\":", stdout);
    json_print_escaped(result->reply);
    fputs(",\"model\":", stdout);
    json_print_escaped(result->model);
    printf(",\"latencyMs\":%lu", (unsigned long)result->latency_ms);
    fputs(",\"status\":", stdout);
    json_print_escaped(result->status);
    printf(",\"httpStatus\":%d", result->http_status);
    fputs("}", stdout);
    response_end();
    free(message);
    free(result);
}

static void handle_ai_assistant_chat(const char *line, const char *request_id, const char *command)
{
    fridge_ai_assistant_request_t *assistant_request = calloc(1, sizeof(*assistant_request));
    fridge_ai_assistant_result_t *result = calloc(1, sizeof(*result));
    fridge_ai_context_preview_t *preview = calloc(1, sizeof(*preview));
    fridge_ai_chat_history_item_t *history = calloc(FRIDGE_AI_MAX_CHAT_HISTORY, sizeof(*history));
    fridge_storage_chat_history_t *storage_history = calloc(1, sizeof(*storage_history));
    fridge_storage_chat_message_t persisted_messages[2] = {0};
    size_t history_pruned_count = 0;
    size_t write_pruned_count = 0;
    bool history_persisted = false;
    esp_err_t err = ESP_OK;
    if (!assistant_request || !result || !preview || !history || !storage_history) {
        free(assistant_request);
        free(result);
        free(preview);
        free(history);
        free(storage_history);
        send_error(request_id, command, "AI assistant allocation failed");
        return;
    }

    fridge_ai_task_request_t context_request = {0};
    build_ai_task_request_from_line(line, &context_request);
    json_get_string(line, "message", assistant_request->message, sizeof(assistant_request->message));
    if (assistant_request->message[0] == '\0') {
        strlcpy(assistant_request->message, context_request.user_text, sizeof(assistant_request->message));
    }
    if (assistant_request->message[0] == '\0') {
        free(assistant_request);
        free(result);
        free(preview);
        free(history);
        free(storage_history);
        send_error(request_id, command, "message is required");
        return;
    }
    strlcpy(context_request.user_text, assistant_request->message, sizeof(context_request.user_text));

    err = fridge_storage_get_chat_history(storage_history, &history_pruned_count);
    if (err != ESP_OK) {
        free(assistant_request);
        free(result);
        free(preview);
        free(history);
        free(storage_history);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    err = fridge_ai_context_build_preview(&context_request, preview);
    if (err != ESP_OK) {
        free(assistant_request);
        free(result);
        free(preview);
        free(history);
        free(storage_history);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    strlcpy(assistant_request->task_type, preview->task_type, sizeof(assistant_request->task_type));
    strlcpy(assistant_request->context_json, preview->preview_json, sizeof(assistant_request->context_json));
    assistant_request->history = history;
    assistant_request->history_count = convert_storage_history_to_ai_history(storage_history, history, FRIDGE_AI_MAX_CHAT_HISTORY);
    assistant_request->context_injected = true;
    assistant_request->needs_confirmation = preview->needs_confirmation;
    assistant_request->local_snapshot_version = preview->local_snapshot_version;

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        free(assistant_request);
        free(result);
        free(preview);
        free(history);
        free(storage_history);
        send_error(request_id, command, "AI assistant sync object failed");
        return;
    }

    usb_ai_assistant_job_t job = {
        .request = assistant_request,
        .result = result,
        .err = ESP_FAIL,
        .done = done,
    };
    BaseType_t task_ok = usb_create_worker_task(ai_assistant_worker_task, "ai_assist_worker", USB_AI_WORKER_TASK_STACK, &job, 4, NULL);
    if (task_ok != pdPASS) {
        vSemaphoreDelete(done);
        free(assistant_request);
        free(result);
        free(preview);
        free(history);
        free(storage_history);
        send_error(request_id, command, "AI assistant worker create failed");
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    ESP_LOGI(TAG,
             "USB protocol stack high watermark=%u words after AI assistant",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    err = job.err;
    if (err != ESP_OK) {
        send_error(request_id, command, result->chat.error[0] ? result->chat.error : esp_err_to_name(err));
        free(assistant_request);
        free(result);
        free(preview);
        free(history);
        free(storage_history);
        return;
    }

    fridge_ai_action_result_t action_result = {0};
    char clean_ai_reply[FRIDGE_AI_MAX_REPLY_LEN + 1] = {0};
    char visible_reply[FRIDGE_AI_MAX_REPLY_LEN + 1] = {0};
    bool memory_updated = false;
    err = fridge_storage_apply_memory_directive(result->chat.reply, clean_ai_reply, sizeof(clean_ai_reply), &memory_updated);
    if (err == ESP_OK && clean_ai_reply[0] != '\0') {
        strlcpy(result->chat.reply, clean_ai_reply, sizeof(result->chat.reply));
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "apply AI memory directive failed: %s", esp_err_to_name(err));
    }

    esp_err_t action_err = fridge_ai_actions_strip_directives(result->chat.reply,
                                                              visible_reply,
                                                              sizeof(visible_reply),
                                                              &action_result);
    bool tool_handled = (action_err == ESP_OK && action_result.executed);
    bool tool_feedback = action_result.tool[0] != '\0' && action_result.message[0] != '\0';
    const char *reply_text = (tool_handled || tool_feedback) ? action_result.message : (visible_reply[0] ? visible_reply : result->chat.reply);

    strlcpy(persisted_messages[0].role, "user", sizeof(persisted_messages[0].role));
    strlcpy(persisted_messages[0].content, assistant_request->message, sizeof(persisted_messages[0].content));
    strlcpy(persisted_messages[0].task_type, assistant_request->task_type, sizeof(persisted_messages[0].task_type));

    strlcpy(persisted_messages[1].role, "assistant", sizeof(persisted_messages[1].role));
    strlcpy(persisted_messages[1].content, reply_text, sizeof(persisted_messages[1].content));
    strlcpy(persisted_messages[1].task_type, assistant_request->task_type, sizeof(persisted_messages[1].task_type));
    err = fridge_storage_append_chat_messages(persisted_messages, 2, &write_pruned_count);
    if (err == ESP_OK) {
        history_persisted = true;
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "chat history not persisted because wall clock is not ready yet");
        write_pruned_count += history_pruned_count;
    } else {
        ESP_LOGW(TAG, "persist chat history failed: %s", esp_err_to_name(err));
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"reply\":", stdout);
    json_print_escaped(reply_text);
    fputs(",\"model\":", stdout);
    json_print_escaped(result->chat.model);
    printf(",\"latencyMs\":%lu", (unsigned long)result->chat.latency_ms);
    fputs(",\"status\":", stdout);
    json_print_escaped(result->chat.status);
    printf(",\"httpStatus\":%d", result->chat.http_status);
    fputs(",\"taskType\":", stdout);
    json_print_escaped(result->task_type);
    fputs(",\"contextInjected\":", stdout);
    fputs(result->context_injected ? "true" : "false", stdout);
    printf(",\"localSnapshotVersion\":%lu", (unsigned long)result->local_snapshot_version);
    fputs(",\"needsConfirmation\":", stdout);
    fputs(result->needs_confirmation ? "true" : "false", stdout);
    fputs(",\"historyInjected\":", stdout);
    fputs(assistant_request->history_count > 0 ? "true" : "false", stdout);
    printf(",\"historyCount\":%u", (unsigned)assistant_request->history_count);
    fputs(",\"historyPersisted\":", stdout);
    fputs(history_persisted ? "true" : "false", stdout);
    printf(",\"historyPrunedCount\":%u", (unsigned)(history_pruned_count + write_pruned_count));
    fputs(",\"memoryUpdated\":", stdout);
    fputs(memory_updated ? "true" : "false", stdout);
    fputs(",\"toolExecuted\":", stdout);
    fputs(tool_handled ? "true" : "false", stdout);
    fputs(",\"toolMessage\":", stdout);
    json_print_escaped(tool_feedback ? action_result.message : "");
    fputs("}", stdout);
    response_end();

    free(assistant_request);
    free(result);
    free(preview);
    free(history);
    free(storage_history);
}

static const char *audio_state_text(fridge_audio_state_t state)
{
    switch (state) {
    case FRIDGE_AUDIO_STATE_WAKE_LISTENING:
        return "wake_listening";
    case FRIDGE_AUDIO_STATE_RECORDING:
        return "recording";
    case FRIDGE_AUDIO_STATE_READY:
        return "ready";
    case FRIDGE_AUDIO_STATE_ERROR:
        return "error";
    case FRIDGE_AUDIO_STATE_IDLE:
    default:
        return "idle";
    }
}

static void print_wake_status_payload(void)
{
    fridge_wake_word_status_t status = {0};
    (void)fridge_wake_word_get_status(&status);
    fputs("{\"enabled\":", stdout);
    fputs(status.enabled ? "true" : "false", stdout);
    fputs(",\"state\":", stdout);
    json_print_escaped(fridge_wake_word_state_text(status.state));
    fputs(",\"wakeWord\":", stdout);
    json_print_escaped(status.wake_word);
    fputs(",\"model\":", stdout);
    json_print_escaped(status.model);
    printf(",\"triggerCount\":%lu,\"lastTriggerMs\":%lu,\"vadState\":%ld,"
           "\"rms\":%ld,\"peakAbs\":%ld,\"timeoutCount\":%lu",
           (unsigned long)status.trigger_count,
           (unsigned long)status.last_trigger_ms,
           (long)status.vad_state,
           (long)status.rms,
           (long)status.peak_abs,
           (unsigned long)status.timeout_count);
    fputs(",\"error\":", stdout);
    json_print_escaped(status.error);
    fputs("}", stdout);
}

static void emit_wake_detected_event(void *user_ctx)
{
    (void)user_ctx;
    event_begin("wake_word_detected");
    fputs(",\"payload\":", stdout);
    print_wake_status_payload();
    event_end();
}

static void handle_wake_status(const char *request_id, const char *command)
{
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_wake_status_payload();
    response_end();
}

static void handle_wake_start(const char *request_id, const char *command)
{
    esp_err_t err = fridge_wake_word_start();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_wake_status(request_id, command);
}

static void handle_wake_stop(const char *request_id, const char *command)
{
    esp_err_t err = fridge_wake_word_stop();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_wake_status(request_id, command);
}

static void handle_wake_reset_stats(const char *request_id, const char *command)
{
    esp_err_t err = fridge_wake_word_reset_stats();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_wake_status(request_id, command);
}

static void print_voice_status_payload(void)
{
    fridge_audio_status_t status = {0};
    (void)fridge_audio_get_status(&status);
    fputs("{\"state\":", stdout);
    json_print_escaped(audio_state_text(status.state));
    printf(",\"durationMs\":%lu,\"pcmBytes\":%u,\"rms\":%ld",
           (unsigned long)status.duration_ms,
           (unsigned)status.pcm_bytes,
           (long)status.rms);
    printf(",\"sampleCount\":%lu,\"peakAbs\":%ld,\"minSample\":%d,\"maxSample\":%d,"
           "\"meanSample\":%ld,\"clipCount\":%lu,\"timeoutCount\":%lu",
           (unsigned long)status.sample_count,
           (long)status.peak_abs,
           (int)status.min_sample,
           (int)status.max_sample,
           (long)status.mean_sample,
           (unsigned long)status.clip_count,
           (unsigned long)status.timeout_count);
    fputs(",\"qualityHint\":", stdout);
    json_print_escaped(status.quality_hint);
    fputs(",\"error\":", stdout);
    json_print_escaped(status.error);
    fputs("}", stdout);
}

static void handle_voice_chat_status(const char *request_id, const char *command)
{
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_voice_status_payload();
    response_end();
}

static const char *speaker_state_text(fridge_speaker_state_t state)
{
    switch (state) {
    case FRIDGE_SPEAKER_STATE_SYNTHESIZING:
        return "synthesizing";
    case FRIDGE_SPEAKER_STATE_PLAYING:
        return "playing";
    case FRIDGE_SPEAKER_STATE_DONE:
        return "done";
    case FRIDGE_SPEAKER_STATE_ERROR:
        return "error";
    case FRIDGE_SPEAKER_STATE_IDLE:
    default:
        return "idle";
    }
}

static void print_tts_status_payload(const fridge_speaker_status_t *status)
{
    fputs("{\"state\":", stdout);
    json_print_escaped(speaker_state_text(status->state));
    printf(",\"sampleRate\":%lu,\"audioBytes\":%u,\"playedBytes\":%u,\"durationMs\":%lu,"
           "\"latencyMs\":%lu,\"httpStatus\":%d",
           (unsigned long)status->sample_rate,
           (unsigned)status->audio_bytes,
           (unsigned)status->played_bytes,
           (unsigned long)status->duration_ms,
           (unsigned long)status->latency_ms,
           status->http_status);
    fputs(",\"model\":", stdout);
    json_print_escaped(status->model);
    fputs(",\"voice\":", stdout);
    json_print_escaped(status->voice);
    fputs(",\"error\":", stdout);
    json_print_escaped(status->error);
    fputs("}", stdout);
}

static void handle_tts_status(const char *request_id, const char *command)
{
    fridge_speaker_status_t status = {0};
    esp_err_t err = fridge_speaker_get_status(&status);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_tts_status_payload(&status);
    response_end();
}

static void handle_tts_stop(const char *request_id, const char *command)
{
    esp_err_t err = fridge_speaker_stop();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_tts_status(request_id, command);
}

static void handle_tts_play(const char *line, const char *request_id, const char *command)
{
    char text[FRIDGE_TTS_MAX_TEXT_LEN + 1] = {0};
    json_get_string(line, "text", text, sizeof(text));
    if (text[0] == '\0') {
        send_error(request_id, command, "text is required");
        return;
    }

    // TTS 播放会通过扬声器回灌到麦克风；播放前暂停本地唤醒监听，避免误触发和 I2S 资源争用。
    esp_err_t wake_err = fridge_wake_word_stop();
    if (wake_err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(wake_err));
        return;
    }

    fridge_speaker_status_t status = {0};
    esp_err_t err = fridge_speaker_synthesize_and_play(text, &status);
    if (err != ESP_OK) {
        fridge_speaker_get_status(&status);
        response_begin(request_id, command, false);
        fputs(",\"error\":", stdout);
        json_print_escaped(status.error[0] ? status.error : esp_err_to_name(err));
        fputs(",\"payload\":", stdout);
        print_tts_status_payload(&status);
        response_end();
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_tts_status_payload(&status);
    response_end();
}

static void handle_voice_chat_start(const char *request_id, const char *command)
{
    esp_err_t err = fridge_audio_start_recording();
    if (err != ESP_OK) {
        send_error(request_id, command, err == ESP_ERR_INVALID_STATE ? "audio busy" : esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_voice_status_payload();
    response_end();
}

static void handle_mic_record_stop(const char *request_id, const char *command)
{
    esp_err_t err = fridge_audio_stop_recording();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_voice_status_payload();
    response_end();
}

static void handle_mic_record_wav(const char *request_id, const char *command)
{
    const int16_t *pcm = NULL;
    size_t pcm_bytes = 0;
    uint32_t duration_ms = 0;
    esp_err_t err = fridge_audio_get_pcm(&pcm, &pcm_bytes, &duration_ms);
    if (err != ESP_OK) {
        send_error(request_id, command, err == ESP_ERR_INVALID_STATE ? "no recorded PCM ready" : esp_err_to_name(err));
        return;
    }
    if (!pcm || pcm_bytes == 0 || pcm_bytes > FRIDGE_AUDIO_MAX_PCM_BYTES) {
        send_error(request_id, command, "recorded PCM size invalid");
        return;
    }

    const size_t wav_bytes = pcm_bytes + 44;
    uint8_t *wav = usb_large_alloc(wav_bytes);
    if (!wav) {
        send_error(request_id, command, "WAV allocation failed");
        return;
    }

    write_wav_header(wav, pcm_bytes);
    memcpy(wav + 44, pcm, pcm_bytes);
    char *wav_b64 = base64_encode_alloc(wav, wav_bytes);
    free(wav);
    if (!wav_b64) {
        send_error(request_id, command, "WAV base64 allocation failed");
        return;
    }

    response_begin(request_id, command, true);
    printf(",\"payload\":{\"sampleRate\":%d,\"channels\":1,\"bitsPerSample\":16,\"durationMs\":%lu,\"pcmBytes\":%u,\"wavBytes\":%u,\"wavBase64\":\"",
           FRIDGE_AUDIO_SAMPLE_RATE,
           (unsigned long)duration_ms,
           (unsigned)pcm_bytes,
           (unsigned)wav_bytes);
    fputs(wav_b64, stdout);
    fputs("\"}", stdout);
    response_end();
    free(wav_b64);
}

static void handle_voice_chat_stop(const char *request_id, const char *command)
{
    esp_err_t err = fridge_audio_stop_recording();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    fridge_voice_session_result_t *result = calloc(1, sizeof(*result));
    if (!result) {
        send_error(request_id, command, "voice chat allocation failed");
        return;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        free(result);
        send_error(request_id, command, "voice chat sync object failed");
        return;
    }

    usb_voice_chat_job_t job = {
        .result = result,
        .err = ESP_FAIL,
        .done = done,
    };
    BaseType_t task_ok = usb_create_worker_task(voice_chat_worker_task, "voice_chat_worker", USB_AI_WORKER_TASK_STACK, &job, 4, NULL);
    if (task_ok != pdPASS) {
        vSemaphoreDelete(done);
        free(result);
        send_error(request_id, command, "voice chat worker create failed");
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    if (job.err != ESP_OK) {
        const char *message = result->error[0] ? result->error : esp_err_to_name(job.err);
        free(result);
        send_error(request_id, command, message);
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"transcript\":", stdout);
    json_print_escaped(result->transcript);
    fputs(",\"reply\":", stdout);
    json_print_escaped(result->reply);
    fputs(",\"asrModel\":", stdout);
    json_print_escaped(result->asr_model);
    fputs(",\"aiModel\":", stdout);
    json_print_escaped(result->ai_model);
    printf(",\"asrLatencyMs\":%lu,\"aiLatencyMs\":%lu,\"asrHttpStatus\":%d,\"aiHttpStatus\":%d,\"audioBytes\":%u,\"historyPrunedCount\":%u}",
           (unsigned long)result->asr_latency_ms,
           (unsigned long)result->ai_latency_ms,
           result->asr_http_status,
           result->ai_http_status,
           (unsigned)result->audio_bytes,
           (unsigned)result->history_pruned_count);
    response_end();

    free(result);
}

static void print_camera_status_payload(const fridge_camera_status_t *status)
{
    fputs("{\"initialized\":", stdout);
    fputs(status->initialized ? "true" : "false", stdout);
    fputs(",\"hasFrame\":", stdout);
    fputs(status->has_frame ? "true" : "false", stdout);
    printf(",\"width\":%d,\"height\":%d,\"jpegBytes\":%u,\"captureMs\":%lu,\"frameId\":%lu,"
           "\"freeHeapKb\":%lu,\"freePsramKb\":%lu",
           status->width,
           status->height,
           (unsigned)status->jpeg_bytes,
           (unsigned long)status->capture_ms,
           (unsigned long)status->frame_id,
           (unsigned long)status->free_heap_kb,
           (unsigned long)status->free_psram_kb);
    fputs(",\"pixelFormat\":", stdout);
    json_print_escaped(status->pixel_format);
    fputs(",\"frameSize\":", stdout);
    json_print_escaped(status->frame_size);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(status->last_error);
    fputs("}", stdout);
}

static void handle_get_camera_status(const char *request_id, const char *command)
{
    fridge_camera_status_t status = {0};
    fridge_camera_get_status(&status);

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_camera_status_payload(&status);
    response_end();
}

static void handle_camera_probe(const char *request_id, const char *command)
{
    fridge_camera_probe_result_t result = {0};
    esp_err_t err = fridge_camera_probe(&result);

    response_begin(request_id, command, err == ESP_OK);
    fputs(",\"payload\":{", stdout);
    fputs("\"ok\":", stdout);
    fputs(result.ok ? "true" : "false", stdout);
    fputs(",\"xclkEnabled\":", stdout);
    fputs(result.xclk_enabled ? "true" : "false", stdout);
    fputs(",\"sccbReady\":", stdout);
    fputs(result.sccb_ready ? "true" : "false", stdout);
    printf(",\"address\":%u", (unsigned)result.sccb_address);
    printf(",\"pidHigh\":%u", (unsigned)result.pid_high);
    printf(",\"pidLow\":%u", (unsigned)result.pid_low);
    printf(",\"pid\":%u", (unsigned)result.pid);
    printf(",\"expectedPid\":%u", (unsigned)result.expected_pid);
    printf(",\"durationMs\":%lu", (unsigned long)result.duration_ms);
    printf(",\"espErr\":%d", (int)err);
    fputs(",\"espErrName\":", stdout);
    json_print_escaped(esp_err_to_name(err));
    fputs(",\"lastError\":", stdout);
    json_print_escaped(result.last_error);
    fputs("}", stdout);
    response_end();
}

static void handle_camera_capture(const char *request_id, const char *command)
{
    esp_err_t err = fridge_camera_capture();
    fridge_camera_status_t status = {0};
    fridge_camera_get_status(&status);
    if (err != ESP_OK) {
        send_error(request_id, command, status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }

    fridge_camera_frame_view_t frame = {0};
    err = fridge_camera_get_frame(&frame);
    if (err != ESP_OK) {
        send_error(request_id, command, status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }

    char *image_b64 = NULL;
    bool preview_omitted = frame.len > CONFIG_FRIDGE_CAMERA_PREVIEW_MAX_BYTES;
    if (!preview_omitted) {
        image_b64 = base64_encode_alloc(frame.data, frame.len);
        if (!image_b64) {
            send_error(request_id, command, "camera preview base64 allocation failed");
            return;
        }
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{", stdout);
    fputs("\"status\":", stdout);
    print_camera_status_payload(&status);
    fputs(",\"previewDataUrl\":", stdout);
    if (image_b64) {
        putchar('"');
        fputs(FRIDGE_CAMERA_DATA_URL_PREFIX, stdout);
        fputs(image_b64, stdout);
        putchar('"');
    } else {
        fputs("null", stdout);
    }
    fputs(",\"previewOmitted\":", stdout);
    fputs(preview_omitted ? "true" : "false", stdout);
    printf(",\"previewMaxBytes\":%d", CONFIG_FRIDGE_CAMERA_PREVIEW_MAX_BYTES);
    fputs("}", stdout);
    response_end();
    free(image_b64);
}

static void handle_camera_jpeg_diag(const char *request_id, const char *command)
{
    esp_err_t err = fridge_camera_capture_hardware_jpeg_diag();
    fridge_camera_status_t status = {0};
    fridge_camera_get_status(&status);
    if (err != ESP_OK) {
        send_error(request_id, command, status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }

    fridge_camera_frame_view_t frame = {0};
    err = fridge_camera_get_frame(&frame);
    if (err != ESP_OK) {
        send_error(request_id, command, status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }

    char *image_b64 = NULL;
    bool preview_omitted = frame.len > CONFIG_FRIDGE_CAMERA_PREVIEW_MAX_BYTES;
    if (!preview_omitted) {
        image_b64 = base64_encode_alloc(frame.data, frame.len);
        if (!image_b64) {
            send_error(request_id, command, "hardware jpeg preview base64 allocation failed");
            return;
        }
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{", stdout);
    fputs("\"status\":", stdout);
    print_camera_status_payload(&status);
    fputs(",\"previewDataUrl\":", stdout);
    if (image_b64) {
        putchar('"');
        fputs(FRIDGE_CAMERA_DATA_URL_PREFIX, stdout);
        fputs(image_b64, stdout);
        putchar('"');
    } else {
        fputs("null", stdout);
    }
    fputs(",\"previewOmitted\":", stdout);
    fputs(preview_omitted ? "true" : "false", stdout);
    printf(",\"previewMaxBytes\":%d", CONFIG_FRIDGE_CAMERA_PREVIEW_MAX_BYTES);
    fputs("}", stdout);
    response_end();
    free(image_b64);
}

static void handle_camera_rgb565_diag(const char *request_id, const char *command)
{
    fridge_camera_diag_result_t result = {0};
    esp_err_t err = fridge_camera_capture_rgb565_diag(&result);

    response_begin(request_id, command, err == ESP_OK);
    fputs(",\"payload\":{", stdout);
    fputs("\"ok\":", stdout);
    fputs(result.ok ? "true" : "false", stdout);
    printf(",\"width\":%d,\"height\":%d,\"bytes\":%u,\"captureMs\":%lu,\"checksum\":%lu",
           result.width,
           result.height,
           (unsigned)result.bytes,
           (unsigned long)result.capture_ms,
           (unsigned long)result.checksum);
    fputs(",\"firstBytes\":\"", stdout);
    for (size_t i = 0; i < result.first_len; i++) {
        printf("%02x", result.first_bytes[i]);
    }
    fputs("\",\"espErr\":", stdout);
    printf("%d", (int)err);
    fputs(",\"espErrName\":", stdout);
    json_print_escaped(esp_err_to_name(err));
    fputs(",\"lastError\":", stdout);
    json_print_escaped(result.last_error);
    fputs("}", stdout);
    response_end();
}

static void handle_camera_analyze(const char *request_id, const char *command)
{
    fridge_ai_image_result_t *result = calloc(1, sizeof(*result));
    if (!result) {
        send_error(request_id, command, "camera analyze allocation failed");
        return;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        free(result);
        send_error(request_id, command, "camera analyze sync object failed");
        return;
    }

    usb_camera_analyze_job_t job = {
        .result = result,
        .err = ESP_FAIL,
        .done = done,
    };
    BaseType_t task_ok = usb_create_worker_task(camera_analyze_worker_task, "camera_ai_worker", USB_CAMERA_WORKER_TASK_STACK, &job, 4, NULL);
    if (task_ok != pdPASS) {
        vSemaphoreDelete(done);
        free(result);
        send_error(request_id, command, "camera analyze worker create failed");
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    if (job.err != ESP_OK) {
        const char *message = result->chat.error[0] ? result->chat.error : esp_err_to_name(job.err);
        free(result);
        send_error(request_id, command, message);
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"taskType\":", stdout);
    json_print_escaped(result->task_type);
    fputs(",\"reply\":", stdout);
    json_print_escaped(result->chat.reply);
    fputs(",\"model\":", stdout);
    json_print_escaped(result->chat.model);
    fputs(",\"status\":", stdout);
    json_print_escaped(result->chat.status);
    printf(",\"httpStatus\":%d,\"latencyMs\":%lu,\"width\":%d,\"height\":%d,\"jpegBytes\":%u,\"needsConfirmation\":%s}",
           result->chat.http_status,
           (unsigned long)result->chat.latency_ms,
           result->width,
           result->height,
           (unsigned)result->jpeg_bytes,
           result->needs_confirmation ? "true" : "false");
    response_end();
    free(result);
}

static void handle_clear_camera_frame(const char *request_id, const char *command)
{
    esp_err_t err = fridge_camera_clear_frame();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_camera_status(request_id, command);
}

static void handle_camera_reset(const char *request_id, const char *command)
{
    esp_err_t err = fridge_camera_reset();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_camera_status(request_id, command);
}

static void handle_get_status(const char *request_id, const char *command)
{
    fridge_device_status_t status = {0};
    fridge_diagnostics_get_status(&status);
    fridge_mqtt_status_t mqtt_status = {0};
    fridge_mqtt_get_status(&mqtt_status);

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"model\":", stdout);
    json_print_escaped(status.model);
    fputs(",\"chip\":", stdout);
    json_print_escaped(status.chip);
    fputs(",\"firmware\":", stdout);
    json_print_escaped(status.firmware);
    fputs(",\"uptime\":", stdout);
    json_print_escaped(status.uptime);
    fputs(",\"flash\":", stdout);
    json_print_escaped(status.flash);
    fputs(",\"psram\":", stdout);
    json_print_escaped(status.psram);
    printf(",\"freeHeapKb\":%lu,\"minHeapKb\":%lu,\"freePsramKb\":%lu,\"temperatureC\":null",
           (unsigned long)status.free_heap_kb,
           (unsigned long)status.min_heap_kb,
           (unsigned long)status.free_psram_kb);
    fputs(",\"wifi\":", stdout);
    json_print_escaped(status.wifi_health);
    fputs(",\"mqtt\":", stdout);
    json_print_escaped(mqtt_status.connected ? "ok" : (mqtt_status.configured ? "warn" : "offline"));
    fputs(",\"usb\":", stdout);
    json_print_escaped(status.usb_health);
    fputs(",\"ota\":", stdout);
    json_print_escaped(status.ota_health);
    fputs(",\"page\":", stdout);
    json_print_escaped(status.page);
    fputs(",\"powerNote\":", stdout);
    json_print_escaped(status.power_note);
    fputs(",\"tasks\":[", stdout);
    for (size_t i = 0; i < status.task_count; i++) {
        if (i > 0) {
            putchar(',');
        }
        fputs("{\"name\":", stdout);
        json_print_escaped(status.tasks[i].name);
        fputs(",\"priority\":", stdout);
        json_print_escaped(status.tasks[i].priority);
        fputs(",\"state\":", stdout);
        json_print_escaped(status.tasks[i].state);
        fputs(",\"heartbeat\":", stdout);
        json_print_escaped(status.tasks[i].heartbeat);
        fputs("}", stdout);
    }
    fputs("]}", stdout);
    response_end();
}

static void handle_get_pins(const char *request_id, const char *command)
{
    static const char *live_pins =
        "["
        "{\"gpio\":\"GPIO1\",\"signal\":\"LIGHT_AO\",\"usage\":\"light sensor ADC\",\"level\":\"safe\",\"note\":\"AO uses 3.3V only, ADC1_CH0 input, do not feed 5V into GPIO.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO4/5\",\"signal\":\"I2C_SCCB\",\"usage\":\"FT6336U touch / MPU6050 / OV3660 SCCB\",\"level\":\"caution\",\"note\":\"SDA=GPIO4, SCL=GPIO5; all pull-ups must go to 3.3V.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO6/7\",\"signal\":\"LCD_WAIT_RST\",\"usage\":\"TR230S WAIT# / RESET#\",\"level\":\"caution\",\"note\":\"WAIT#=GPIO6, RESET#=GPIO7; screen VCC is 5V but logic remains 3.3V.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO9/10/11/12/13/14\",\"signal\":\"LCD_QSPI\",\"usage\":\"TR230S QSPI bus\",\"level\":\"caution\",\"note\":\"D3=GPIO9, CS=GPIO10, D0=GPIO11, SCLK=GPIO12, D1=GPIO13, D2=GPIO14.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO15\",\"signal\":\"TOUCH_INT\",\"usage\":\"FT6336U touch interrupt\",\"level\":\"safe\",\"note\":\"TP_INT=GPIO15; TP_RST is not connected in the N8R8-safe wiring.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO17/18/8/3/46/48/45/16\",\"signal\":\"CAM_D0_D7\",\"usage\":\"OV3660 8-bit DVP data\",\"level\":\"caution\",\"note\":\"D0-D7=GPIO17/18/8/3/46/48/45/16; GPIO3/45/46 are strap-sensitive and GPIO48 shares the onboard RGB LED.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO2/38/19/47\",\"signal\":\"CAM_SYNC_CLK\",\"usage\":\"OV3660 VSYNC/HREF/PCLK/XCLK\",\"level\":\"caution\",\"note\":\"VSYNC=GPIO2, HREF=GPIO38, PCLK=GPIO19, XCLK=GPIO47; GPIO35/36/37 are forbidden on N8R8.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO40/41/42/39\",\"signal\":\"I2S_AUDIO\",\"usage\":\"INMP441 microphone and MAX98357A speaker\",\"level\":\"caution\",\"note\":\"BCLK=GPIO40 and WS=GPIO41 are shared; MIC_SD=GPIO42, SPK_DIN=GPIO39. Avoid simultaneous record/play in first bring-up.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO21/20\",\"signal\":\"RADAR_UART\",\"usage\":\"24GHz radar UART\",\"level\":\"safe\",\"note\":\"Radar TX -> GPIO21, ESP32 TX GPIO20 -> radar RX; OT2 is not connected because GPIO19 is camera PCLK.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO35/36/37\",\"signal\":\"FORBIDDEN\",\"usage\":\"N8R8 Flash/PSRAM related\",\"level\":\"danger\",\"note\":\"Do not connect peripherals here. These pins can cause MSPI tuning failure and boot reset loops.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO0\",\"signal\":\"RESERVED_STRAP\",\"usage\":\"reserved strapping pin\",\"level\":\"danger\",\"note\":\"Not used in the N8R8-safe wiring; avoid camera DVP here.\",\"readonly\":true}"
        "]";
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    fputs(live_pins, stdout);
    response_end();
}

static esp_err_t i2c_probe_addr(uint8_t addr)
{
    // 只发送地址不写寄存器，用于判断 I2C 总线上是否有设备 ACK。
    // 注意：GPIO4/GPIO5 总线同时可能挂 FT6336U、MPU6050 和 OV3660 SCCB，上拉必须到 3.3V。
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = i2c_master_start(cmd);
    if (ret == ESP_OK) {
        ret = i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    }
    if (ret == ESP_OK) {
        ret = i2c_master_stop(cmd);
    }
    if (ret == ESP_OK) {
        ret = i2c_master_cmd_begin(FRIDGE_IMU_I2C_PORT, cmd, pdMS_TO_TICKS(40));
    }
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t i2c_read_u8(uint8_t addr, uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(FRIDGE_IMU_I2C_PORT,
                                        addr,
                                        &reg,
                                        1,
                                        value,
                                        1,
                                        pdMS_TO_TICKS(60));
}

static void print_i2c_reg_probe(uint8_t addr, uint8_t reg)
{
    uint8_t value = 0;
    esp_err_t err = i2c_read_u8(addr, reg, &value);
    fputs("{\"reg\":", stdout);
    printf("%u,\"ok\":%s,\"value\":%u,\"hex\":\"0x%02X\",\"err\":",
           (unsigned)reg,
           err == ESP_OK ? "true" : "false",
           (unsigned)(err == ESP_OK ? value : 0),
           (unsigned)(err == ESP_OK ? value : 0));
    json_print_escaped(esp_err_to_name(err));
    fputs("}", stdout);
}

static void print_i2c_candidate_probe(uint8_t addr)
{
    static const uint8_t regs[] = {
        0x00, // FT 系列模式寄存器，若读通可辅助判断是否真是触摸控制器。
        0x02, // FT 系列触点状态寄存器。
        0x03, // FT 系列第一触点 X 高位寄存器。
        0x80, // FT 系列阈值/配置区常见寄存器。
        0x88, // FT 系列中断模式常见寄存器。
        0xA3, // FT6336U Chip ID。
        0xA8, // FT6336U Vendor ID。
    };

    fputs("{\"addr\":", stdout);
    printf("%u,\"hex\":\"0x%02X\",\"ack\":%s,\"regs\":[",
           (unsigned)addr,
           (unsigned)addr,
           i2c_probe_addr(addr) == ESP_OK ? "true" : "false");
    for (size_t i = 0; i < sizeof(regs); i++) {
        if (i > 0) {
            putchar(',');
        }
        print_i2c_reg_probe(addr, regs[i]);
    }
    fputs("]}", stdout);
}

static void handle_touch_i2c_diag(const char *request_id, const char *command)
{
    // 触摸诊断命令：扫描 I2C0，并重点读取 FT6336U/MPU6050 的身份寄存器。
    // 它不改变业务状态，适合只接屏幕时判断触摸芯片是否真的在 GPIO4/GPIO5 上应答。
    (void)fridge_sensors_init();

    bool found_any = false;
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"port\":%d,\"sda\":%d,\"scl\":%d,\"touchAddr\":%u,\"imuAddr\":%u,\"addresses\":[",
           FRIDGE_IMU_I2C_PORT,
           FRIDGE_IMU_I2C_SDA_GPIO,
           FRIDGE_IMU_I2C_SCL_GPIO,
           FRIDGE_TOUCH_I2C_ADDR,
           FRIDGE_IMU_DEFAULT_ADDR);
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_probe_addr(addr) == ESP_OK) {
            if (found_any) {
                putchar(',');
            }
            printf("%u", (unsigned)addr);
            found_any = true;
        }
    }
    fputs("],\"foundAny\":", stdout);
    fputs(found_any ? "true" : "false", stdout);

    uint8_t value = 0;
    esp_err_t touch_chip = i2c_read_u8(FRIDGE_TOUCH_I2C_ADDR, 0xA3, &value);
    fputs(",\"ft6336u\":{\"chipIdOk\":", stdout);
    fputs(touch_chip == ESP_OK ? "true" : "false", stdout);
    printf(",\"chipId\":%u,\"chipIdErr\":", (unsigned)(touch_chip == ESP_OK ? value : 0));
    json_print_escaped(esp_err_to_name(touch_chip));
    value = 0;
    esp_err_t touch_vendor = i2c_read_u8(FRIDGE_TOUCH_I2C_ADDR, 0xA8, &value);
    printf(",\"vendorId\":%u,\"vendorErr\":", (unsigned)(touch_vendor == ESP_OK ? value : 0));
    json_print_escaped(esp_err_to_name(touch_vendor));
    value = 0;
    esp_err_t touch_points = i2c_read_u8(FRIDGE_TOUCH_I2C_ADDR, 0x02, &value);
    printf(",\"tdStatus\":%u,\"tdStatusErr\":", (unsigned)(touch_points == ESP_OK ? value : 0));
    json_print_escaped(esp_err_to_name(touch_points));
    fputs("}", stdout);

    value = 0;
    esp_err_t imu_who = i2c_read_u8(FRIDGE_IMU_DEFAULT_ADDR, 0x75, &value);
    fputs(",\"mpu6050\":{\"whoAmIOk\":", stdout);
    fputs(imu_who == ESP_OK ? "true" : "false", stdout);
    printf(",\"whoAmI\":%u,\"err\":", (unsigned)(imu_who == ESP_OK ? value : 0));
    json_print_escaped(esp_err_to_name(imu_who));
    fputs("}", stdout);

    static const uint8_t candidates[] = {
        FRIDGE_TOUCH_I2C_ADDR,
        FRIDGE_IMU_DEFAULT_ADDR,
    };
    fputs(",\"candidates\":[", stdout);
    for (size_t i = 0; i < sizeof(candidates); i++) {
        if (i > 0) {
            putchar(',');
        }
        print_i2c_candidate_probe(candidates[i]);
    }
    fputs("]", stdout);

    fputs(",\"hint\":", stdout);
    if (!found_any) {
        json_print_escaped("No I2C ACK on GPIO4/GPIO5. Check TP-VCC=3V3, TP-GND, SDA/SCL direction, and pull-ups to 3.3V.");
    } else if (touch_chip != ESP_OK) {
        json_print_escaped("I2C bus has devices, but the verified FT6336U/TRN0706B address 0x48 did not answer. Check panel touch pin order, FPC direction, TP_RST, and pull-ups.");
    } else {
        json_print_escaped("FT6336U/TRN0706B answered at verified addr 0x48; tap the UI to verify coordinate direction.");
    }
    fputs("}", stdout);
    response_end();
}

static const char *radar_mode_label(fridge_radar_mode_t mode)
{
    switch (mode) {
    case FRIDGE_RADAR_MODE_NORMAL:
        return "normal";
    case FRIDGE_RADAR_MODE_REPORT:
        return "report";
    case FRIDGE_RADAR_MODE_ERROR:
        return "error";
    case FRIDGE_RADAR_MODE_IDLE:
    default:
        return "idle";
    }
}

static const char *radar_zone_label(fridge_radar_distance_zone_t zone)
{
    switch (zone) {
    case FRIDGE_RADAR_DISTANCE_NEAR:
        return "near";
    case FRIDGE_RADAR_DISTANCE_MID:
        return "mid";
    case FRIDGE_RADAR_DISTANCE_FAR:
        return "far";
    case FRIDGE_RADAR_DISTANCE_UNKNOWN:
    default:
        return "unknown";
    }
}

static void print_radar_payload(const fridge_radar_snapshot_t *radar)
{
    fputs("{\"ready\":", stdout);
    fputs(radar->ready ? "true" : "false", stdout);
    fputs(",\"mode\":", stdout);
    json_print_escaped(radar_mode_label(radar->mode));
    fputs(",\"presence\":", stdout);
    fputs(radar->presence ? "true" : "false", stdout);
    fputs(",\"nearClutter\":", stdout);
    fputs(radar->near_clutter ? "true" : "false", stdout);
    fputs(",\"staticClutter\":", stdout);
    fputs(radar->static_clutter ? "true" : "false", stdout);
    fputs(",\"humanCandidate\":", stdout);
    fputs(radar->human_candidate ? "true" : "false", stdout);
    fputs(",\"stablePresence\":", stdout);
    fputs(radar->stable_presence ? "true" : "false", stdout);
    fputs(",\"within1m\":", stdout);
    fputs(radar->within_1m ? "true" : "false", stdout);
    fputs(",\"approaching\":", stdout);
    fputs(radar->approaching ? "true" : "false", stdout);
    fputs(",\"thresholdPresence\":", stdout);
    fputs(radar->threshold_presence ? "true" : "false", stdout);
    printf(",\"distanceRaw\":%u,\"smoothedDistanceRaw\":%u,"
           "\"peakGate\":%u,\"peakEnergy\":%u,\"estimatedGate\":%u,\"stableGate\":%u,\"thresholdGate\":%u,"
           "\"confidence\":%u,\"stability\":%u,\"approachScore\":%u,"
           "\"approachFrames\":%u,\"approachDistanceDelta\":%u,"
           "\"motionScore\":%u,\"distanceSpan\":%u,\"gateSpan\":%u,\"energyChangeScore\":%u,"
           "\"staticScore\":%u,\"humanScore\":%u,"
           "\"thresholdScore\":%u,\"holdFramesRemaining\":%u,"
           "\"nearEnergy\":%u,\"midEnergy\":%u,\"farEnergy\":%u,"
           "\"frameCount\":%lu,\"parseErrorCount\":%lu,\"timeoutCount\":%lu,\"ot2Level\":%d",
           (unsigned)radar->distance_raw,
           (unsigned)radar->smoothed_distance_raw,
           (unsigned)radar->peak_gate,
           (unsigned)radar->peak_energy,
           (unsigned)radar->estimated_gate,
           (unsigned)radar->stable_gate,
           (unsigned)radar->threshold_gate,
           (unsigned)radar->confidence,
           (unsigned)radar->stability,
           (unsigned)radar->approach_score,
           (unsigned)radar->approach_frames,
           (unsigned)radar->approach_distance_delta,
           (unsigned)radar->motion_score,
           (unsigned)radar->distance_span,
           (unsigned)radar->gate_span,
           (unsigned)radar->energy_change_score,
           (unsigned)radar->static_score,
           (unsigned)radar->human_score,
           (unsigned)radar->threshold_score,
           (unsigned)radar->hold_frames_remaining,
           (unsigned)radar->near_energy,
           (unsigned)radar->mid_energy,
           (unsigned)radar->far_energy,
           (unsigned long)radar->frame_count,
           (unsigned long)radar->parse_error_count,
           (unsigned long)radar->timeout_count,
           radar->ot2_level);
    fputs(",\"stableZone\":", stdout);
    json_print_escaped(radar_zone_label(radar->stable_zone));
    fputs(",\"gateEnergy\":[", stdout);
    for (size_t i = 0; i < FRIDGE_RADAR_GATE_COUNT; i++) {
        if (i > 0) {
            putchar(',');
        }
        printf("%u", (unsigned)radar->gate_energy[i]);
    }
    fputs("],\"lastText\":", stdout);
    json_print_escaped(radar->last_text);
    fputs(",\"targetClass\":", stdout);
    json_print_escaped(radar->target_class);
    fputs(",\"rejectionReason\":", stdout);
    json_print_escaped(radar->rejection_reason);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(radar->last_error);
    printf(",\"updatedAtMs\":%lld}", (long long)radar->updated_at_ms);
}

static void handle_radar_test_start(const char *request_id, const char *command)
{
    esp_err_t err = fridge_radar_start_report_mode();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    fridge_radar_snapshot_t radar = {0};
    (void)fridge_radar_get_snapshot(&radar);
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_radar_payload(&radar);
    response_end();
}

static void handle_radar_test_stop(const char *request_id, const char *command)
{
    esp_err_t err = fridge_radar_start_normal_mode();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    fridge_radar_snapshot_t radar = {0};
    (void)fridge_radar_get_snapshot(&radar);
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_radar_payload(&radar);
    response_end();
}

static void handle_radar_test_status(const char *request_id, const char *command)
{
    fridge_radar_snapshot_t radar = {0};
    (void)fridge_radar_get_snapshot(&radar);
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_radar_payload(&radar);
    response_end();
}

static void handle_get_sensors(const char *request_id, const char *command)
{
    fridge_sensor_snapshot_t sensors = {0};
    fridge_sm_snapshot_t sm = {0};
    (void)fridge_sensors_get_snapshot(&sensors);
    bool has_sm = fridge_state_machine_get_snapshot(&sm) == ESP_OK;

    response_begin(request_id, command, true);
    // lux 字段为旧 Web 面板兼容值，实际不是 BH1750 物理 lux；当前按反向光敏 AO 换算后的亮度 0-1023 返回。
    printf(",\"payload\":{\"pir\":false,\"lux\":%u,\"lightRaw12bit\":%u,\"lightValue10bit\":%u,"
           "\"lightPercent\":%u,\"lightDelta\":%d,\"lightPolarity\":\"raw_high_dark\","
           "\"imuReady\":%s,\"imuAddress\":%u,\"imuWhoAmI\":%u,\"imuError\":%d,"
           "\"accelXG\":%.4f,\"accelYG\":%.4f,\"accelZG\":%.4f,"
           "\"gyroXDps\":%.4f,\"gyroYDps\":%.4f,\"gyroZDps\":%.4f,"
           "\"imuTemperatureC\":%.2f,\"pitchDeg\":%.2f,\"rollDeg\":%.2f,"
           "\"angleDelta\":%.2f,\"vibrationPeak\":%.4f,",
           (unsigned)sensors.light_value_10bit,
           (unsigned)sensors.light_raw_12bit,
           (unsigned)sensors.light_value_10bit,
           (unsigned)sensors.light_percent,
           (int)sensors.light_delta,
           sensors.imu_ready ? "true" : "false",
           (unsigned)sensors.imu_address,
           (unsigned)sensors.imu_who_am_i,
           sensors.imu_error,
           (double)sensors.accel_x_g,
           (double)sensors.accel_y_g,
           (double)sensors.accel_z_g,
           (double)sensors.gyro_x_dps,
           (double)sensors.gyro_y_dps,
           (double)sensors.gyro_z_dps,
           (double)sensors.imu_temperature_c,
           (double)sensors.pitch_deg,
           (double)sensors.roll_deg,
           (double)sensors.angle_delta,
           (double)sensors.vibration_peak);
    fputs("\"radar\":", stdout);
    print_radar_payload(&sensors.radar);
    putchar(',');
    fputs("\"touch\":\"not_connected\",\"display\":\"not_connected\",\"buzzer\":\"not_connected\",\"doorState\":", stdout);
    json_print_escaped(has_sm ? fridge_state_machine_door_to_string(sm.door_state) : "UNKNOWN");
    fputs(",\"stateMachine\":", stdout);
    if (has_sm) {
        print_state_machine_payload(&sm);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"updatedAt\":", stdout);
    if (sensors.ready) {
        printf("\"%lld ms\"", (long long)sensors.updated_at_ms);
    } else {
        json_print_escaped("ADC warming up");
    }
    fputs("}", stdout);
    response_end();
}

static void handle_get_diagnostics(const char *request_id, const char *command)
{
    fridge_diagnostic_snapshot_t diag = {0};
    fridge_diagnostics_get_snapshot(&diag);

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"psram\":", stdout);
    json_print_escaped(diag.psram);
    fputs(",\"flashPartition\":", stdout);
    json_print_escaped(diag.flash_partition);
    fputs(",\"littlefs\":", stdout);
    json_print_escaped(diag.littlefs);
    fputs(",\"otaSlot\":", stdout);
    json_print_escaped(diag.ota_slot);
    printf(",\"brownoutCount\":%lu,\"watchdogCount\":%lu",
           (unsigned long)diag.brownout_count,
           (unsigned long)diag.watchdog_count);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(diag.last_error);
    fputs(",\"riskItems\":["
          "\"屏幕 VCC 为 5V，GPIO 逻辑为 3.3V，不得把 5V 信号接入 GPIO。\","
          "\"Wi-Fi 扫描和连接期间有电流峰值，请使用稳定 USB/5V 供电。\","
          "\"本面板不提供 GPIO 输出控制，避免误触发真实硬件。\"]}", stdout);
    response_end();
}

static void handle_get_logs(const char *request_id, const char *command)
{
    response_begin(request_id, command, true);
    fputs(",\"payload\":[{\"id\":\"boot\",\"at\":\"runtime\",\"level\":\"info\",\"source\":\"firmware\",\"message\":\"USB JSON protocol running\"}]", stdout);
    response_end();
}

static void handle_line(const char *line)
{
    char request_id[USB_VALUE_BUFFER_SIZE] = "unknown";
    char command[USB_VALUE_BUFFER_SIZE] = "";

    // 串口上可能混入人工输入或其他工具残留文本；只处理 JSON Lines 请求，避免把噪声回成错误响应。
    if (!line || line[0] != '{') {
        return;
    }

    json_get_string(line, "request_id", request_id, sizeof(request_id));
    if (!json_get_string(line, "command", command, sizeof(command))) {
        // 兼容早期调试脚本可能使用的 cmd 字段，WebUI 仍统一发送 command。
        json_get_string(line, "cmd", command, sizeof(command));
    }

    if (command[0] == '\0') {
        send_error(request_id, "unknown", "command is required");
        return;
    }

    if (strcmp(command, "get_status") == 0) {
        handle_get_status(request_id, command);
    } else if (strcmp(command, "get_network") == 0) {
        handle_get_network(request_id, command);
    } else if (strcmp(command, "scan_wifi") == 0) {
        handle_scan_wifi(request_id, command);
    } else if (strcmp(command, "set_network") == 0) {
        handle_set_network(line, request_id, command);
    } else if (strcmp(command, "get_mqtt_config") == 0 || strcmp(command, "get_mqtt_status") == 0) {
        handle_get_mqtt_config(request_id, command);
    } else if (strcmp(command, "set_mqtt_config") == 0) {
        handle_set_mqtt_config(line, request_id, command);
    } else if (strcmp(command, "clear_mqtt_secret") == 0) {
        handle_clear_mqtt_secret(request_id, command);
    } else if (strcmp(command, "mqtt_publish_state") == 0) {
        handle_mqtt_publish_state(request_id, command);
    } else if (strcmp(command, "get_ai_config") == 0) {
        handle_get_ai_config(request_id, command);
    } else if (strcmp(command, "get_ai_profiles") == 0) {
        handle_get_ai_profiles(request_id, command);
    } else if (strcmp(command, "set_ai_config") == 0) {
        handle_set_ai_config(line, request_id, command);
    } else if (strcmp(command, "clear_ai_key") == 0) {
        handle_clear_ai_key(request_id, command);
    } else if (strcmp(command, "get_asr_config") == 0) {
        handle_get_asr_config(request_id, command);
    } else if (strcmp(command, "set_asr_config") == 0) {
        handle_set_asr_config(line, request_id, command);
    } else if (strcmp(command, "clear_asr_key") == 0) {
        handle_clear_asr_key(request_id, command);
    } else if (strcmp(command, "get_tts_config") == 0) {
        handle_get_tts_config(request_id, command);
    } else if (strcmp(command, "set_tts_config") == 0) {
        handle_set_tts_config(line, request_id, command);
    } else if (strcmp(command, "clear_tts_key") == 0) {
        handle_clear_tts_key(request_id, command);
    } else if (strcmp(command, "get_state_machine_config") == 0) {
        handle_get_state_machine_config(request_id, command);
    } else if (strcmp(command, "set_state_machine_config") == 0) {
        handle_set_state_machine_config(line, request_id, command);
    } else if (strcmp(command, "get_state_machine_status") == 0) {
        handle_get_state_machine_status(request_id, command);
    } else if (strcmp(command, "create_ai_profile") == 0) {
        handle_create_ai_profile(line, request_id, command);
    } else if (strcmp(command, "select_ai_profile") == 0) {
        handle_select_ai_profile(line, request_id, command);
    } else if (strcmp(command, "delete_ai_profile") == 0) {
        handle_delete_ai_profile(line, request_id, command);
    } else if (strcmp(command, "test_ai_chat") == 0) {
        handle_test_ai_chat(line, request_id, command);
    } else if (strcmp(command, "ai_assistant_chat") == 0) {
        handle_ai_assistant_chat(line, request_id, command);
    } else if (strcmp(command, "get_kitchen_tools") == 0) {
        handle_get_kitchen_tools(request_id, command);
    } else if (strcmp(command, "timer_start") == 0) {
        handle_timer_start(line, request_id, command);
    } else if (strcmp(command, "timer_pause") == 0) {
        handle_kitchen_simple(request_id, command, fridge_kitchen_tools_timer_pause);
    } else if (strcmp(command, "timer_resume") == 0) {
        handle_kitchen_simple(request_id, command, fridge_kitchen_tools_timer_resume);
    } else if (strcmp(command, "timer_cancel") == 0) {
        handle_kitchen_simple(request_id, command, fridge_kitchen_tools_timer_cancel);
    } else if (strcmp(command, "stopwatch_start") == 0) {
        handle_kitchen_simple(request_id, command, fridge_kitchen_tools_stopwatch_start);
    } else if (strcmp(command, "stopwatch_pause") == 0) {
        handle_kitchen_simple(request_id, command, fridge_kitchen_tools_stopwatch_pause);
    } else if (strcmp(command, "stopwatch_reset") == 0) {
        handle_kitchen_simple(request_id, command, fridge_kitchen_tools_stopwatch_reset);
    } else if (strcmp(command, "alarm_set") == 0) {
        handle_alarm_set(line, request_id, command);
    } else if (strcmp(command, "alarm_cancel") == 0) {
        handle_alarm_id_action(line, request_id, command, fridge_kitchen_tools_alarm_cancel);
    } else if (strcmp(command, "alarm_dismiss") == 0) {
        handle_alarm_id_action(line, request_id, command, fridge_kitchen_tools_alarm_dismiss);
    } else if (strcmp(command, "wake_start") == 0) {
        handle_wake_start(request_id, command);
    } else if (strcmp(command, "wake_stop") == 0) {
        handle_wake_stop(request_id, command);
    } else if (strcmp(command, "wake_status") == 0) {
        handle_wake_status(request_id, command);
    } else if (strcmp(command, "wake_reset_stats") == 0) {
        handle_wake_reset_stats(request_id, command);
    } else if (strcmp(command, "mic_record_start") == 0) {
        handle_voice_chat_start(request_id, command);
    } else if (strcmp(command, "mic_record_status") == 0) {
        handle_voice_chat_status(request_id, command);
    } else if (strcmp(command, "mic_record_stop") == 0) {
        handle_mic_record_stop(request_id, command);
    } else if (strcmp(command, "mic_record_wav") == 0) {
        handle_mic_record_wav(request_id, command);
    } else if (strcmp(command, "voice_chat_start") == 0) {
        handle_voice_chat_start(request_id, command);
    } else if (strcmp(command, "voice_chat_stop") == 0) {
        handle_voice_chat_stop(request_id, command);
    } else if (strcmp(command, "voice_chat_status") == 0) {
        handle_voice_chat_status(request_id, command);
    } else if (strcmp(command, "tts_play") == 0) {
        handle_tts_play(line, request_id, command);
    } else if (strcmp(command, "tts_status") == 0) {
        handle_tts_status(request_id, command);
    } else if (strcmp(command, "tts_stop") == 0) {
        handle_tts_stop(request_id, command);
    } else if (strcmp(command, "radar_test_start") == 0) {
        handle_radar_test_start(request_id, command);
    } else if (strcmp(command, "radar_test_status") == 0) {
        handle_radar_test_status(request_id, command);
    } else if (strcmp(command, "radar_test_stop") == 0) {
        handle_radar_test_stop(request_id, command);
    } else if (strcmp(command, "get_camera_status") == 0) {
        handle_get_camera_status(request_id, command);
    } else if (strcmp(command, "camera_probe") == 0) {
        handle_camera_probe(request_id, command);
    } else if (strcmp(command, "camera_capture") == 0) {
        handle_camera_capture(request_id, command);
    } else if (strcmp(command, "camera_jpeg_diag") == 0) {
        handle_camera_jpeg_diag(request_id, command);
    } else if (strcmp(command, "camera_rgb565_diag") == 0) {
        handle_camera_rgb565_diag(request_id, command);
    } else if (strcmp(command, "camera_analyze") == 0) {
        handle_camera_analyze(request_id, command);
    } else if (strcmp(command, "clear_camera_frame") == 0) {
        handle_clear_camera_frame(request_id, command);
    } else if (strcmp(command, "camera_reset") == 0) {
        handle_camera_reset(request_id, command);
    } else if (strcmp(command, "get_ai_context_preview") == 0) {
        handle_get_ai_context_preview(line, request_id, command);
    } else if (strcmp(command, "test_ai_task") == 0) {
        handle_test_ai_task(line, request_id, command);
    } else if (strcmp(command, "get_memory_summary") == 0) {
        handle_get_memory_summary(request_id, command);
    } else if (strcmp(command, "set_memory_summary") == 0) {
        handle_set_memory_summary(line, request_id, command);
    } else if (strcmp(command, "clear_memory_summary") == 0) {
        handle_clear_memory_summary(request_id, command);
    } else if (strcmp(command, "get_chat_history") == 0) {
        handle_get_chat_history(request_id, command);
    } else if (strcmp(command, "clear_chat_history") == 0) {
        handle_clear_chat_history(request_id, command);
    } else if (strcmp(command, "restore_seed_inventory") == 0) {
        handle_restore_seed_inventory(request_id, command);
    } else if (strcmp(command, "get_pins") == 0) {
        handle_get_pins(request_id, command);
    } else if (strcmp(command, "touch_i2c_diag") == 0) {
        handle_touch_i2c_diag(request_id, command);
    } else if (strcmp(command, "get_sensors") == 0) {
        handle_get_sensors(request_id, command);
    } else if (strcmp(command, "get_diagnostics") == 0) {
        handle_get_diagnostics(request_id, command);
    } else if (strcmp(command, "get_logs") == 0) {
        handle_get_logs(request_id, command);
    } else {
        send_error(request_id, command, "unknown command");
    }
}

static void usb_protocol_task(void *arg)
{
    (void)arg;
    char *line = s_usb_line_buffer;
    size_t line_len = 0;

    ESP_LOGI(TAG, "USB JSON Lines protocol task started");
    while (true) {
        // ESP-IDF 的 USB/UART stdin 可能按可用字节返回，不能假设 fgets 一定拿到完整一行。
        // 这里逐字节累积到换行后再解析 JSON Lines，避免把半条请求误判为 command is required。
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (line_len == 0) {
                continue;
            }
            line[line_len] = '\0';
            handle_line(line);
            line_len = 0;
            continue;
        }

        if (line_len + 1 >= USB_LINE_BUFFER_SIZE) {
            ESP_LOGW(TAG, "USB JSON line too long, dropping buffered data");
            line_len = 0;
            continue;
        }

        line[line_len++] = (char)ch;
    }
}

esp_err_t fridge_usb_protocol_start(void)
{
    // 唤醒词事件通过同一条 USB JSON Lines 输出；这里只注册轻量回调，不在回调里触发 ASR/AI。
    (void)fridge_wake_word_set_event_callback(emit_wake_detected_event, NULL);
    BaseType_t ok = xTaskCreate(usb_protocol_task, "usb_protocol", USB_PROTOCOL_TASK_STACK, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
