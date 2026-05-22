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
#include "esp_err.h"
#include "esp_log.h"
#include "fridge_ai_client.h"
#include "fridge_ai_context.h"
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "fridge_diagnostics.h"
#include "fridge_network.h"
#include "fridge_sensors.h"
#include "fridge_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define USB_LINE_BUFFER_SIZE 16384
#define USB_VALUE_BUFFER_SIZE 128
#define USB_SCAN_MAX_AP 50
#define USB_PROTOCOL_TASK_STACK 12288
#define USB_AI_WORKER_TASK_STACK 32768

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
    fridge_asr_result_t *asr;
    fridge_ai_assistant_result_t *ai;
    fridge_ai_assistant_request_t *request;
    fridge_ai_chat_history_item_t *history;
    fridge_storage_chat_history_t *storage_history;
    fridge_storage_chat_message_t *persisted_messages;
    size_t *history_pruned_count;
    esp_err_t err;
    SemaphoreHandle_t done;
} usb_voice_chat_job_t;

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
        // 历史注入只使用已经完成的 user/assistant 轮次；最后一条 user 可能是上次异常中断遗留，不能再次发送给模型。
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
            // OpenAI-compatible 服务对 history 顺序更严格，遇到坏轮次时宁可不注入，避免拖垮真实 API 请求。
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

static void send_error(const char *request_id, const char *command, const char *error)
{
    response_begin(request_id, command, false);
    fputs(",\"error\":", stdout);
    json_print_escaped(error);
    fputs(",\"payload\":{}", stdout);
    response_end();
}

static void print_network_payload(void)
{
    fridge_network_status_t status = {0};
    fridge_network_get_status(&status);

    fputs("{\"ssid\":", stdout);
    json_print_escaped(status.ssid);
    fputs(",\"wifiPassword\":\"\",\"mqttHost\":\"\",\"apiBaseUrl\":\"\",\"ntpServer\":", stdout);
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
    job->err = fridge_asr_transcribe_latest_recording(job->asr);
    if (job->err == ESP_OK) {
        if (job->asr->text[0] == '\0' || strlen(job->asr->text) < 2) {
            // ASR 偶发返回空文本或极短噪声时不要继续请求 AI，避免兼容服务因为空 user content 返回 HTTP 400。
            strlcpy(job->asr->error, "ASR 转写为空或过短，请重新录音", sizeof(job->asr->error));
            job->err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (job->err == ESP_OK) {
        fridge_ai_task_request_t context_request = {
            .include_inventory = true,
            .include_memory = true,
            .include_reminders = true,
            .include_preferences = true,
        };
        strlcpy(context_request.task_type, "voice_intent_parse", sizeof(context_request.task_type));
        strlcpy(context_request.user_text, job->asr->text, sizeof(context_request.user_text));

        fridge_ai_context_preview_t *preview = calloc(1, sizeof(*preview));
        if (!preview) {
            job->err = ESP_ERR_NO_MEM;
        } else {
            if (job->storage_history && job->history && job->history_pruned_count) {
                job->err = fridge_storage_get_chat_history(job->storage_history, job->history_pruned_count);
            }
            if (job->err == ESP_OK) {
                job->err = fridge_ai_context_build_preview(&context_request, preview);
            }
            if (job->err == ESP_OK) {
                strlcpy(job->request->message, job->asr->text, sizeof(job->request->message));
                strlcpy(job->request->task_type, "voice_intent_parse", sizeof(job->request->task_type));
                strlcpy(job->request->context_json, preview->preview_json, sizeof(job->request->context_json));
                job->request->history = job->history;
                job->request->history_count = convert_storage_history_to_ai_history(job->storage_history, job->history, FRIDGE_AI_MAX_CHAT_HISTORY);
                job->request->context_injected = true;
                job->request->needs_confirmation = preview->needs_confirmation;
                job->request->local_snapshot_version = preview->local_snapshot_version;
                job->err = fridge_ai_client_assistant_chat(job->request, job->ai);
            }
            free(preview);
        }
    }

    if (job->err == ESP_OK && job->persisted_messages && job->history_pruned_count) {
        snprintf(job->persisted_messages[0].id, sizeof(job->persisted_messages[0].id), "msg_%" PRIu32 "_u", (uint32_t)(xTaskGetTickCount() & 0xFFFFFF));
        strlcpy(job->persisted_messages[0].role, "user", sizeof(job->persisted_messages[0].role));
        strlcpy(job->persisted_messages[0].content, job->asr->text, sizeof(job->persisted_messages[0].content));
        strlcpy(job->persisted_messages[0].task_type, "voice_intent_parse", sizeof(job->persisted_messages[0].task_type));
        snprintf(job->persisted_messages[1].id, sizeof(job->persisted_messages[1].id), "msg_%" PRIu32 "_a", (uint32_t)((xTaskGetTickCount() + 1) & 0xFFFFFF));
        strlcpy(job->persisted_messages[1].role, "assistant", sizeof(job->persisted_messages[1].role));
        strlcpy(job->persisted_messages[1].content, job->ai->chat.reply, sizeof(job->persisted_messages[1].content));
        strlcpy(job->persisted_messages[1].task_type, "voice_intent_parse", sizeof(job->persisted_messages[1].task_type));
        (void)fridge_storage_append_chat_messages(job->persisted_messages, 2, job->history_pruned_count);
    }

    ESP_LOGI(TAG, "voice chat worker done, stack high watermark=%u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));
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
    BaseType_t task_ok = xTaskCreate(ai_chat_worker_task, "ai_chat_worker", USB_AI_WORKER_TASK_STACK, &job, 4, NULL);
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
    BaseType_t task_ok = xTaskCreate(ai_assistant_worker_task, "ai_assist_worker", USB_AI_WORKER_TASK_STACK, &job, 4, NULL);
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

    strlcpy(persisted_messages[0].role, "user", sizeof(persisted_messages[0].role));
    strlcpy(persisted_messages[0].content, assistant_request->message, sizeof(persisted_messages[0].content));
    strlcpy(persisted_messages[0].task_type, assistant_request->task_type, sizeof(persisted_messages[0].task_type));

    strlcpy(persisted_messages[1].role, "assistant", sizeof(persisted_messages[1].role));
    strlcpy(persisted_messages[1].content, result->chat.reply, sizeof(persisted_messages[1].content));
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
    json_print_escaped(result->chat.reply);
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

static void handle_voice_chat_start(const char *request_id, const char *command)
{
    esp_err_t err = fridge_audio_start_recording();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
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

static void handle_voice_chat_stop(const char *request_id, const char *command)
{
    esp_err_t err = fridge_audio_stop_recording();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    fridge_asr_result_t *asr = calloc(1, sizeof(*asr));
    fridge_ai_assistant_result_t *ai = calloc(1, sizeof(*ai));
    fridge_ai_assistant_request_t *assistant_request = calloc(1, sizeof(*assistant_request));
    fridge_ai_chat_history_item_t *history = calloc(FRIDGE_AI_MAX_CHAT_HISTORY, sizeof(*history));
    fridge_storage_chat_history_t *storage_history = calloc(1, sizeof(*storage_history));
    fridge_storage_chat_message_t *persisted_messages = calloc(2, sizeof(*persisted_messages));
    size_t history_pruned_count = 0;
    if (!asr || !ai || !assistant_request || !history || !storage_history || !persisted_messages) {
        free(asr);
        free(ai);
        free(assistant_request);
        free(history);
        free(storage_history);
        free(persisted_messages);
        send_error(request_id, command, "voice chat allocation failed");
        return;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        free(asr);
        free(ai);
        free(assistant_request);
        free(history);
        free(storage_history);
        free(persisted_messages);
        send_error(request_id, command, "voice chat sync object failed");
        return;
    }

    usb_voice_chat_job_t job = {
        .asr = asr,
        .ai = ai,
        .request = assistant_request,
        .history = history,
        .storage_history = storage_history,
        .persisted_messages = persisted_messages,
        .history_pruned_count = &history_pruned_count,
        .err = ESP_FAIL,
        .done = done,
    };
    BaseType_t task_ok = xTaskCreate(voice_chat_worker_task, "voice_chat_worker", USB_AI_WORKER_TASK_STACK, &job, 4, NULL);
    if (task_ok != pdPASS) {
        vSemaphoreDelete(done);
        free(asr);
        free(ai);
        free(assistant_request);
        free(history);
        free(storage_history);
        free(persisted_messages);
        send_error(request_id, command, "voice chat worker create failed");
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    if (job.err != ESP_OK) {
        const char *message = asr->error[0] ? asr->error : (ai->chat.error[0] ? ai->chat.error : esp_err_to_name(job.err));
        free(asr);
        free(ai);
        free(assistant_request);
        free(history);
        free(storage_history);
        free(persisted_messages);
        send_error(request_id, command, message);
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"transcript\":", stdout);
    json_print_escaped(asr->text);
    fputs(",\"reply\":", stdout);
    json_print_escaped(ai->chat.reply);
    fputs(",\"asrModel\":", stdout);
    json_print_escaped(asr->model);
    fputs(",\"aiModel\":", stdout);
    json_print_escaped(ai->chat.model);
    printf(",\"asrLatencyMs\":%lu,\"aiLatencyMs\":%lu,\"asrHttpStatus\":%d,\"aiHttpStatus\":%d,\"audioBytes\":%u,\"historyPrunedCount\":%u}",
           (unsigned long)asr->latency_ms,
           (unsigned long)ai->chat.latency_ms,
           asr->http_status,
           ai->chat.http_status,
           (unsigned)asr->audio_bytes,
           (unsigned)history_pruned_count);
    response_end();

    free(asr);
    free(ai);
    free(assistant_request);
    free(history);
    free(storage_history);
    free(persisted_messages);
}

static void handle_get_status(const char *request_id, const char *command)
{
    fridge_device_status_t status = {0};
    fridge_diagnostics_get_status(&status);

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
    json_print_escaped(status.mqtt_health);
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
        "{\"gpio\":\"GPIO40\",\"signal\":\"MIC_SCK\",\"usage\":\"INMP441 I2S BCLK\",\"level\":\"safe\",\"note\":\"I2S microphone clock, keep wiring short and common GND.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO41\",\"signal\":\"MIC_WS\",\"usage\":\"INMP441 I2S WS\",\"level\":\"safe\",\"note\":\"Word select / LRCLK for 16kHz mono capture.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO42\",\"signal\":\"MIC_SD\",\"usage\":\"INMP441 I2S data\",\"level\":\"safe\",\"note\":\"Data output from microphone to ESP32-S3 input.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO10/12/11/13/14/9\",\"signal\":\"LCD_QSPI\",\"usage\":\"screen bus\",\"level\":\"caution\",\"note\":\"Screen pins remain reserved for TR230S QSPI.\",\"readonly\":true},"
        "{\"gpio\":\"GPIO0/35-37/45/46\",\"signal\":\"RESERVED\",\"usage\":\"boot or flash/psram sensitive\",\"level\":\"danger\",\"note\":\"Do not connect new peripherals here before hardware review.\",\"readonly\":true}"
        "]";
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    fputs(live_pins, stdout);
    response_end();
}

static void handle_get_sensors(const char *request_id, const char *command)
{
    fridge_sensor_snapshot_t sensors = {0};
    (void)fridge_sensors_get_snapshot(&sensors);

    response_begin(request_id, command, true);
    // lux 字段为旧 Web 面板兼容值，实际不是 BH1750 物理 lux；当前按反向光敏 AO 换算后的亮度 0-1023 返回。
    printf(",\"payload\":{\"pir\":false,\"lux\":%u,\"lightRaw12bit\":%u,\"lightValue10bit\":%u,"
           "\"lightPercent\":%u,\"lightDelta\":%d,\"lightPolarity\":\"raw_high_dark\","
           "\"angleDelta\":0,\"vibrationPeak\":0,",
           (unsigned)sensors.light_value_10bit,
           (unsigned)sensors.light_raw_12bit,
           (unsigned)sensors.light_value_10bit,
           (unsigned)sensors.light_percent,
           (int)sensors.light_delta);
    fputs("\"touch\":\"not_connected\",\"display\":\"not_connected\",\"buzzer\":\"not_connected\",\"doorState\":\"IDLE\",\"updatedAt\":", stdout);
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
    } else if (strcmp(command, "mic_record_start") == 0) {
        handle_voice_chat_start(request_id, command);
    } else if (strcmp(command, "mic_record_status") == 0) {
        handle_voice_chat_status(request_id, command);
    } else if (strcmp(command, "mic_record_stop") == 0) {
        handle_mic_record_stop(request_id, command);
    } else if (strcmp(command, "voice_chat_start") == 0) {
        handle_voice_chat_start(request_id, command);
    } else if (strcmp(command, "voice_chat_stop") == 0) {
        handle_voice_chat_stop(request_id, command);
    } else if (strcmp(command, "voice_chat_status") == 0) {
        handle_voice_chat_status(request_id, command);
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
    } else if (strcmp(command, "get_pins") == 0) {
        handle_get_pins(request_id, command);
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
    BaseType_t ok = xTaskCreate(usb_protocol_task, "usb_protocol", USB_PROTOCOL_TASK_STACK, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
