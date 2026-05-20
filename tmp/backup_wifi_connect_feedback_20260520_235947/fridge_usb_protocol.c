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
#include "fridge_diagnostics.h"
#include "fridge_network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define USB_LINE_BUFFER_SIZE 4096
#define USB_VALUE_BUFFER_SIZE 128
#define USB_SCAN_MAX_AP 50

static const char *TAG = "usb_protocol";

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

static void response_begin(const char *request_id, const char *command, bool ok)
{
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
        send_error(request_id, command, esp_err_to_name(err));
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

static void handle_get_ai_config(const char *request_id, const char *command)
{
    fridge_ai_config_view_t config = {0};
    esp_err_t err = fridge_ai_client_get_config(&config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_ai_config_payload(&config);
    response_end();
}

static void handle_get_ai_profiles(const char *request_id, const char *command)
{
    fridge_ai_profile_list_t profiles = {0};
    esp_err_t err = fridge_ai_client_get_profiles(&profiles);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_ai_profiles_payload(&profiles);
    response_end();
}

static void handle_set_ai_config(const char *line, const char *request_id, const char *command)
{
    fridge_ai_config_view_t current = {0};
    esp_err_t err = fridge_ai_client_get_config(&current);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    fridge_ai_config_update_t update = {0};
    update.profile_id = (uint8_t)json_get_u32(line, "profileId", current.profile_id);
    strlcpy(update.profile_name, current.profile_name, sizeof(update.profile_name));
    json_get_string(line, "profileName", update.profile_name, sizeof(update.profile_name));
    strlcpy(update.api_base_url, current.api_base_url, sizeof(update.api_base_url));
    strlcpy(update.model, current.model[0] ? current.model : FRIDGE_AI_DEFAULT_MODEL, sizeof(update.model));
    strlcpy(update.system_prompt, current.system_prompt, sizeof(update.system_prompt));
    update.timeout_ms = current.timeout_ms ? current.timeout_ms : FRIDGE_AI_DEFAULT_TIMEOUT_MS;

    json_get_string(line, "apiBaseUrl", update.api_base_url, sizeof(update.api_base_url));
    json_get_string(line, "model", update.model, sizeof(update.model));
    json_get_string(line, "systemPrompt", update.system_prompt, sizeof(update.system_prompt));
    update.timeout_ms = json_get_u32(line, "timeoutMs", update.timeout_ms);

    if (json_has_key(line, "apiKey")) {
        json_get_string(line, "apiKey", update.api_key, sizeof(update.api_key));
        update.update_api_key = update.api_key[0] != '\0';
    }

    err = fridge_ai_client_set_config(&update);
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

static void handle_test_ai_chat(const char *line, const char *request_id, const char *command)
{
    char message[FRIDGE_AI_MAX_CHAT_MESSAGE_LEN + 1] = {0};
    json_get_string(line, "message", message, sizeof(message));
    if (message[0] == '\0') {
        send_error(request_id, command, "message is required");
        return;
    }

    fridge_ai_chat_result_t result = {0};
    esp_err_t err = fridge_ai_client_test_chat(message, &result);
    if (err != ESP_OK) {
        send_error(request_id, command, result.error[0] ? result.error : esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"reply\":", stdout);
    json_print_escaped(result.reply);
    fputs(",\"model\":", stdout);
    json_print_escaped(result.model);
    printf(",\"latencyMs\":%lu", (unsigned long)result.latency_ms);
    fputs(",\"status\":", stdout);
    json_print_escaped(result.status);
    printf(",\"httpStatus\":%d", result.http_status);
    fputs("}", stdout);
    response_end();
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
    char line[USB_LINE_BUFFER_SIZE];
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

        if (line_len + 1 >= sizeof(line)) {
            ESP_LOGW(TAG, "USB JSON line too long, dropping buffered data");
            line_len = 0;
            continue;
        }

        line[line_len++] = (char)ch;
    }
}

esp_err_t fridge_usb_protocol_start(void)
{
    BaseType_t ok = xTaskCreate(usb_protocol_task, "usb_protocol", 12288, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
