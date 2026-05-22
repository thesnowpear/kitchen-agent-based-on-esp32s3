// 冰箱小精灵诊断组件。
// 负责读取 heap、PSRAM、Flash、固件版本和网络健康状态；仅做只读诊断，不触碰 GPIO。

#include "fridge_diagnostics.h"

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fridge_network.h"

static int64_t s_boot_time_us;

static void format_uptime(char *buffer, size_t buffer_size)
{
    int64_t elapsed_s = (esp_timer_get_time() - s_boot_time_us) / 1000000;
    int hours = (int)(elapsed_s / 3600);
    int minutes = (int)((elapsed_s % 3600) / 60);
    int seconds = (int)(elapsed_s % 60);
    snprintf(buffer, buffer_size, "%02d:%02d:%02d", hours, minutes, seconds);
}

static void set_task_snapshot(fridge_task_snapshot_t *task,
                              const char *name,
                              const char *priority,
                              const char *state,
                              const char *heartbeat)
{
    // 任务快照要复制字符串内容，不能保存局部状态结构中的指针，否则 USB 响应中会出现乱码。
    if (!task) {
        return;
    }
    strlcpy(task->name, name ? name : "", sizeof(task->name));
    strlcpy(task->priority, priority ? priority : "", sizeof(task->priority));
    strlcpy(task->state, state ? state : "", sizeof(task->state));
    strlcpy(task->heartbeat, heartbeat ? heartbeat : "", sizeof(task->heartbeat));
}

void fridge_diagnostics_init(void)
{
    s_boot_time_us = esp_timer_get_time();
}

esp_err_t fridge_diagnostics_get_status(fridge_device_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const esp_app_desc_t *app_desc = esp_app_get_description();

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    fridge_network_status_t net = {0};
    fridge_network_get_status(&net);

    strlcpy(status->model, "冰箱小精灵 DevKit", sizeof(status->model));
    snprintf(status->chip, sizeof(status->chip), "ESP32-S3 rev %d", chip_info.revision);
    snprintf(status->firmware, sizeof(status->firmware), "%s", app_desc ? app_desc->version : "dev");
    format_uptime(status->uptime, sizeof(status->uptime));
    snprintf(status->flash, sizeof(status->flash), "%lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    snprintf(status->psram, sizeof(status->psram), "%lu KB free", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    status->free_heap_kb = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
    status->min_heap_kb = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024;
    status->free_psram_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    status->page = net.connected ? "WIFI_CONNECTED / USB_CONSOLE" : (net.connecting ? "WIFI_CONNECTING / USB_CONSOLE" : "PROVISIONING / USB_CONSOLE");
    status->power_note = "Wi-Fi 发射有电流峰值；调试时请确认 USB/5V 供电稳定，避免 brownout。";
    status->wifi_health = net.connected ? "ok" : (net.connecting ? "warn" : "offline");
    status->mqtt_health = "offline";
    status->usb_health = "ok";
    status->ota_health = "warn";
    set_task_snapshot(&status->tasks[0], "main_task", "高", "running", "boot");
    set_task_snapshot(&status->tasks[1], "usb_protocol", "中", "running", "stdin");
    set_task_snapshot(&status->tasks[2], "wifi", "中", net.connected ? "connected" : (net.connecting ? "connecting" : "idle"), net.ip);
    status->task_count = 3;
    return ESP_OK;
}

esp_err_t fridge_diagnostics_get_snapshot(fridge_diagnostic_snapshot_t *snapshot)
{
    if (!snapshot) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(snapshot, 0, sizeof(*snapshot));

    fridge_network_status_t net = {0};
    fridge_network_get_status(&net);

    snprintf(snapshot->psram, sizeof(snapshot->psram), "%lu KB free", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    strlcpy(snapshot->flash_partition, "8MB + OTA + LittleFS 由 sdkconfig.defaults/partitions.csv 定义", sizeof(snapshot->flash_partition));
    strlcpy(snapshot->littlefs, "planned: assets + cache", sizeof(snapshot->littlefs));
    strlcpy(snapshot->ota_slot, "OTA 分区已预留，升级逻辑待接入", sizeof(snapshot->ota_slot));
    snapshot->brownout_count = 0;
    snapshot->watchdog_count = 0;
    strlcpy(snapshot->last_error, net.last_error[0] ? net.last_error : "无网络错误", sizeof(snapshot->last_error));
    return ESP_OK;
}
