// 冰箱小精灵 USB JSON Lines 协议组件。
// 负责从串口读取 Web 面板请求，并输出单行 JSON 响应；普通 ESP_LOG 日志仍可并行输出。
// 注意：密码字段只用于调用网络组件，不会在响应和日志中回显。

#include "fridge_usb_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "fridge_ai_client.h"
#include "fridge_ai_context.h"
#include "fridge_diagnostics.h"
#include "fridge_network.h"
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

static size_t json_get_chat_history(const char *json, fridge_ai_chat_history_item_t *history, size_t max_history)
{
    const char *pos = find_json_key(json, "history");
    if (!pos || *pos != '[' || !history || max_history == 0) {
        return 0;
    }
    pos++;
    size_t count = 0;
    while (*pos && *pos != ']' && count < max_history) {
        while (*pos && (isspace((unsigned char)*pos) || *pos == ',')) {
            pos++;
        }
        if (*pos != '{') {
            const char *next = skip_json_value(pos);
            if (!next || next == pos) {
                break;
            }
            pos = next;
            continue;
        }
        const char *item_start = pos;
        const char *item_end = skip_json_value(pos);
        if (!item_end || item_end <= item_start) {
            break;
        }
        size_t item_len = (size_t)(item_end - item_start);
        char *item = calloc(1, item_len + 1);
        if (!item) {
            break;
        }
        memcpy(item, item_start, item_len);
        json_get_string(item, "role", history[count].role, sizeof(history[count].role));
        json_get_string(item, "content", history[count].content, sizeof(history[count].content));
        if (history[count].content[0] != '\0') {
            if (strcmp(history[count].role, "assistant") != 0) {
                strlcpy(history[count].role, "user", sizeof(history[count].role));
            }
            count++;
        }
        free(item);
        pos = item_end;
    }
    return count;
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
    fputs(",\"saved\":", stdout);
    fputs(status.saved ? "true" : "false", stdout);
    fputs(",\"internet\":", stdout);
    fputs(status.internet_ready ? "true" : "false", stdout);
    fputs(",\"status\":", stdout);
    json_print_escaped(status.connected ? (status.internet_ready ? "online" : "wifi_connected") : "offline");
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
    if (!assistant_request || !result || !preview || !history) {
        free(assistant_request);
        free(result);
        free(preview);
        free(history);
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
        send_error(request_id, command, "message is required");
        return;
    }
    strlcpy(context_request.user_text, assistant_request->message, sizeof(context_request.user_text));

    esp_err_t err = fridge_ai_context_build_preview(&context_request, preview);
    if (err != ESP_OK) {
        free(assistant_request);
        free(result);
        free(preview);
        free(history);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    strlcpy(assistant_request->task_type, preview->task_type, sizeof(assistant_request->task_type));
    strlcpy(assistant_request->context_json, preview->preview_json, sizeof(assistant_request->context_json));
    assistant_request->history = history;
    assistant_request->history_count = json_get_chat_history(line, history, FRIDGE_AI_MAX_CHAT_HISTORY);
    assistant_request->context_injected = true;
    assistant_request->needs_confirmation = preview->needs_confirmation;
    assistant_request->local_snapshot_version = preview->local_snapshot_version;

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        free(assistant_request);
        free(result);
        free(preview);
        free(history);
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
        return;
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
    fputs("}", stdout);
    response_end();

    free(assistant_request);
    free(result);
    free(preview);
    free(history);
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
    static const char *pins =
        "["
        "{\"gpio\":\"GPIO10\",\"signal\":\"LCD_CS\",\"usage\":\"屏幕片选\",\"level\":\"safe\",\"note\":\"低有效，QSPI 屏幕专用。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO12\",\"signal\":\"LCD_SCLK\",\"usage\":\"QSPI 时钟\",\"level\":\"caution\",\"note\":\"调试阶段从低时钟起步，不直接冲 100MHz。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO4\",\"signal\":\"I2C_SDA\",\"usage\":\"触摸/光照/IMU\",\"level\":\"caution\",\"note\":\"SDA 上拉到 3.3V，不能上拉到 5V。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO5\",\"signal\":\"I2C_SCL\",\"usage\":\"触摸/光照/IMU\",\"level\":\"caution\",\"note\":\"SCL 上拉到 3.3V。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO0\",\"signal\":\"BOOT\",\"usage\":\"启动绑带脚\",\"level\":\"danger\",\"note\":\"禁止随意外接，会影响下载/启动模式。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO35-37\",\"signal\":\"PSRAM/Flash\",\"usage\":\"保留\",\"level\":\"danger\",\"note\":\"可能被 Flash/PSRAM 占用，首版不使用。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO45/46\",\"signal\":\"STRAP\",\"usage\":\"启动敏感\",\"level\":\"danger\",\"note\":\"启动绑带相关，未经核对不要连接外设。\",\"readonly\":true}"
        "]";
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    fputs(pins, stdout);
    response_end();
}

static void handle_get_sensors(const char *request_id, const char *command)
{
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"pir\":false,\"lux\":0,\"lightDelta\":0,\"angleDelta\":0,\"vibrationPeak\":0,"
          "\"touch\":\"未接入\",\"display\":\"未接入\",\"buzzer\":\"未接入\",\"doorState\":\"BOOT\",\"updatedAt\":\"实时固件\"}", stdout);
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
