#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 任务快照：首版只提供启动、USB 协议和网络三类关键任务，后续可扩展为真实 FreeRTOS 遍历。
typedef struct {
    const char *name;
    const char *priority;
    const char *state;
    const char *heartbeat;
} fridge_task_snapshot_t;

typedef struct {
    char model[32];
    char chip[32];
    char firmware[32];
    char uptime[24];
    char flash[32];
    char psram[32];
    uint32_t free_heap_kb;
    uint32_t min_heap_kb;
    uint32_t free_psram_kb;
    const char *page;
    const char *power_note;
    const char *wifi_health;
    const char *mqtt_health;
    const char *usb_health;
    const char *ota_health;
    fridge_task_snapshot_t tasks[4];
    size_t task_count;
} fridge_device_status_t;

typedef struct {
    char psram[48];
    char flash_partition[80];
    char littlefs[48];
    char ota_slot[48];
    uint32_t brownout_count;
    uint32_t watchdog_count;
    char last_error[96];
} fridge_diagnostic_snapshot_t;

// 初始化诊断模块，记录启动时间。该模块只读系统状态，不改 GPIO 或外设输出。
void fridge_diagnostics_init(void);

// 读取设备状态，供 Web 面板 get_status 使用。
esp_err_t fridge_diagnostics_get_status(fridge_device_status_t *status);

// 读取诊断摘要，供 Web 面板 get_diagnostics 使用。
esp_err_t fridge_diagnostics_get_snapshot(fridge_diagnostic_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
