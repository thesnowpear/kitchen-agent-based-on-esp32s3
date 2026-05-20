// 冰箱小精灵 USB JSON Lines 协议组件。
// 负责从串口读取 Web 面板请求，并输出单行 JSON 响应；普通 ESP_LOG 日志仍可并行输出。
// 注意：密码字段只用于调用网络组件，不会在响应和日志中回显。

#include "fridge_usb_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "fridge_diagnostics.h"
#include "fridge_network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define USB_LINE_BUFFER_SIZE 1024
#define USB_VALUE_BUFFER_SIZE 128
#define USB_SCAN_MAX_AP 20

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
    char pattern[48];
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
    json_get_string(line, "request_id", request_id, sizeof(request_id));
    json_get_string(line, "command", command, sizeof(command));

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

    ESP_LOGI(TAG, "USB JSON Lines protocol task started");
    while (true) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        handle_line(line);
    }
}

esp_err_t fridge_usb_protocol_start(void)
{
    BaseType_t ok = xTaskCreate(usb_protocol_task, "usb_protocol", 6144, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
