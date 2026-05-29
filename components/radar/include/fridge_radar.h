#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_RADAR_GATE_COUNT 16

typedef enum {
    FRIDGE_RADAR_MODE_IDLE = 0,
    FRIDGE_RADAR_MODE_NORMAL = 1,
    FRIDGE_RADAR_MODE_REPORT = 2,
    FRIDGE_RADAR_MODE_ERROR = 3,
} fridge_radar_mode_t;

typedef enum {
    FRIDGE_RADAR_DISTANCE_UNKNOWN = 0,
    FRIDGE_RADAR_DISTANCE_NEAR = 1,
    FRIDGE_RADAR_DISTANCE_MID = 2,
    FRIDGE_RADAR_DISTANCE_FAR = 3,
} fridge_radar_distance_zone_t;

// 24GHz 人体雷达快照：给 Web 面板、USB 调试和后续状态机复用。
// 注意：distance_raw 只表示模块原始距离值，stable_* 是固件按多帧门位能量估算出的调试结果。
// HMMD/LD2410 类模块更适合做“存在检测 + 粗略距离区间”，不应把单帧距离值当成精确测距。
typedef struct {
    bool ready;
    fridge_radar_mode_t mode;
    bool presence;
    bool near_clutter;
    bool static_clutter;
    bool human_candidate;
    uint16_t distance_raw;
    uint16_t gate_energy[FRIDGE_RADAR_GATE_COUNT];
    bool stable_presence;
    bool within_1m;
    bool approaching;
    bool threshold_presence;
    uint8_t peak_gate;
    uint16_t peak_energy;
    uint8_t estimated_gate;
    uint8_t stable_gate;
    uint8_t threshold_gate;
    fridge_radar_distance_zone_t stable_zone;
    uint8_t confidence;
    uint8_t stability;
    uint8_t approach_score;
    uint8_t approach_frames;
    uint16_t approach_distance_delta;
    uint8_t motion_score;
    uint8_t distance_span;
    uint8_t gate_span;
    uint8_t energy_change_score;
    uint8_t static_score;
    uint8_t human_score;
    uint8_t threshold_score;
    uint8_t hold_frames_remaining;
    uint16_t smoothed_distance_raw;
    uint16_t near_energy;
    uint16_t mid_energy;
    uint16_t far_energy;
    uint32_t frame_count;
    uint32_t parse_error_count;
    uint32_t timeout_count;
    int ot2_level;
    char last_text[64];
    char target_class[24];
    char rejection_reason[64];
    char last_error[128];
    int64_t updated_at_ms;
} fridge_radar_snapshot_t;

// 初始化雷达 UART 采集任务。
// 硬件注意：当前测试接线默认使用 UART1，RX=GPIO21，TX=GPIO20，OT2 不接；只接受 3.3V 逻辑，不接 5V。
esp_err_t fridge_radar_init(void);

// 切换到上报模式，开始解析二进制人体检测帧。
esp_err_t fridge_radar_start_report_mode(void);

// 切换到正常模式，保留文本输出作为回退。
esp_err_t fridge_radar_start_normal_mode(void);

// 获取最近一次雷达快照。
esp_err_t fridge_radar_get_snapshot(fridge_radar_snapshot_t *out);

#ifdef __cplusplus
}
#endif
