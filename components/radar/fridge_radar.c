// 冰箱小精灵 24GHz 人体雷达组件。
// 负责通过 UART 接收 HMMD-mmWave-Sensor 的文本或二进制上报帧，并整理成 Web 可视化快照。
// 硬件注意：当前测试接线只使用 3V3、GND、UART_TX、UART_RX；不要把 5V 逻辑直接接入 ESP32-S3。

#include "fridge_radar.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define FRIDGE_RADAR_UART UART_NUM_1
#define FRIDGE_RADAR_UART_BAUDRATE 115200
#define FRIDGE_RADAR_UART_RX_GPIO 21
#define FRIDGE_RADAR_UART_TX_GPIO 20
#define FRIDGE_RADAR_OT2_GPIO GPIO_NUM_NC
#define FRIDGE_RADAR_TASK_STACK 4096
#define FRIDGE_RADAR_READ_CHUNK 128
#define FRIDGE_RADAR_RX_BUFFER 512
#define FRIDGE_RADAR_MIN_FRAME_LEN 10
#define FRIDGE_RADAR_MAX_FRAME_LEN 256
#define FRIDGE_RADAR_GATE_WIDTH_CM 70
#define FRIDGE_RADAR_OPERATOR_GATE_MAX 12
#define FRIDGE_RADAR_HISTORY_COUNT 7
#define FRIDGE_RADAR_MIN_PEAK_ENERGY 100
#define FRIDGE_RADAR_MIN_DOMINANCE_PERCENT 18
#define FRIDGE_RADAR_STABLE_REQUIRED 3
#define FRIDGE_RADAR_NEAR_CLUTTER_GATE_MAX 2
#define FRIDGE_RADAR_REMOTE_GATE_MIN 3
#define FRIDGE_RADAR_REMOTE_PEAK_RATIO_PERCENT 35
#define FRIDGE_RADAR_NEAR_CLUTTER_DISTANCE_RAW_MAX 45
#define FRIDGE_RADAR_NEAR_CLUTTER_REMOTE_RATIO_MAX 8
#define FRIDGE_RADAR_WITHIN_1M_GATE_MAX 1
#define FRIDGE_RADAR_WITHIN_1M_DISTANCE_RAW_MAX 120
#define FRIDGE_RADAR_WITHIN_1M_MIN_CONFIDENCE 60
#define FRIDGE_RADAR_APPROACH_MIN_CONFIDENCE 55
#define FRIDGE_RADAR_APPROACH_MIN_SAMPLES 4
#define FRIDGE_RADAR_MOTION_MIN_SCORE 34
#define FRIDGE_RADAR_HUMAN_CANDIDATE_MIN_SCORE 30
#define FRIDGE_RADAR_RELIABLE_HUMAN_MIN_SCORE 45
#define FRIDGE_RADAR_STABLE_MIN_MOTION_SCORE 28
#define FRIDGE_RADAR_STATIC_CLUTTER_HISTORY_MIN 5
#define FRIDGE_RADAR_STATIC_CLUTTER_MOTION_MAX 22
#define FRIDGE_RADAR_STATIC_CLUTTER_STABILITY_MIN 66
#define FRIDGE_RADAR_STATIC_SCORE_BLOCK_MIN 70
#define FRIDGE_RADAR_APPROACH_MIN_FRAMES 2
#define FRIDGE_RADAR_APPROACH_MIN_DISTANCE_DELTA 25
#define FRIDGE_RADAR_DISTANCE_JITTER_UNIT 8
#define FRIDGE_RADAR_ENERGY_CHANGE_UNIT 18
#define FRIDGE_RADAR_DISAPPEAR_DELAY_FRAMES 30

static const char *TAG = "fridge_radar";

// 参考厂家上位机 appConfig.xml 的默认参数。
// 原值是 dB 阈值，这里提前换算成约等效原始能量，避免 ESP32 每帧做 log10 浮点计算。
static const uint16_t s_trigger_threshold[FRIDGE_RADAR_GATE_COUNT] = {
    59979, 29992, 2999, 2000, 500, 400, 400, 300,
    300, 300, 300, 250, 250, 200, 200, 200,
};

static const uint16_t s_hold_threshold[FRIDGE_RADAR_GATE_COUNT] = {
    39994, 19999, 400, 300, 300, 200, 200, 150,
    150, 100, 100, 100, 100, 100, 100, 100,
};

static const uint8_t s_report_mode_cmd[] = {
    0xFD, 0xFC, 0xFB, 0xFA,
    0x08, 0x00,
    0x12, 0x00,
    0x00, 0x00, 0x04, 0x00,
    0x00, 0x00,
    0x04, 0x03, 0x02, 0x01,
};

static const uint8_t s_normal_mode_cmd[] = {
    0xFD, 0xFC, 0xFB, 0xFA,
    0x08, 0x00,
    0x12, 0x00,
    0x00, 0x00, 0x64, 0x00,
    0x00, 0x00,
    0x04, 0x03, 0x02, 0x01,
};

static const uint8_t s_report_tail_candidates[][4] = {
    {0x08, 0x07, 0x06, 0x05},
    {0xF8, 0xF7, 0xF6, 0xF5},
};

static SemaphoreHandle_t s_lock;
static bool s_initialized;
static bool s_uart_ready;
static char s_rx_buffer[FRIDGE_RADAR_RX_BUFFER];
static size_t s_rx_len;
static fridge_radar_snapshot_t s_snapshot;
static uint8_t s_gate_history[FRIDGE_RADAR_HISTORY_COUNT];
static uint16_t s_distance_history[FRIDGE_RADAR_HISTORY_COUNT];
static uint16_t s_energy_history[FRIDGE_RADAR_HISTORY_COUNT];
static uint8_t s_approach_history[FRIDGE_RADAR_HISTORY_COUNT];
static size_t s_history_index;
static size_t s_history_count;
static bool s_threshold_presence;
static uint8_t s_disappear_frames_remaining;

static uint16_t radar_u32_to_u16(uint32_t value)
{
    return value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}

static fridge_radar_distance_zone_t radar_zone_from_gate(uint8_t gate)
{
    if (gate <= 3) {
        return FRIDGE_RADAR_DISTANCE_NEAR;
    }
    if (gate <= 8) {
        return FRIDGE_RADAR_DISTANCE_MID;
    }
    return FRIDGE_RADAR_DISTANCE_FAR;
}

static const char *radar_zone_text(fridge_radar_distance_zone_t zone)
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

static uint8_t radar_gate_from_distance_raw(uint16_t distance_raw)
{
    uint32_t gate = ((uint32_t)distance_raw + (FRIDGE_RADAR_GATE_WIDTH_CM / 2U)) / FRIDGE_RADAR_GATE_WIDTH_CM;
    if (gate >= FRIDGE_RADAR_GATE_COUNT) {
        gate = FRIDGE_RADAR_GATE_COUNT - 1;
    }
    return (uint8_t)gate;
}

static void radar_reset_filter(void)
{
    memset(s_gate_history, 0, sizeof(s_gate_history));
    memset(s_distance_history, 0, sizeof(s_distance_history));
    memset(s_energy_history, 0, sizeof(s_energy_history));
    memset(s_approach_history, 0, sizeof(s_approach_history));
    s_history_index = 0;
    s_history_count = 0;
    s_threshold_presence = false;
    s_disappear_frames_remaining = 0;
}

static uint16_t radar_average_distance(void)
{
    if (s_history_count == 0) {
        return 0;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i < s_history_count; i++) {
        sum += s_distance_history[i];
    }
    return radar_u32_to_u16(sum / s_history_count);
}

static uint8_t radar_most_common_gate(uint8_t fallback_gate, uint8_t *same_count)
{
    uint8_t best_gate = fallback_gate;
    uint8_t best_count = 0;
    for (size_t i = 0; i < s_history_count; i++) {
        uint8_t candidate = s_gate_history[i];
        uint8_t count = 0;
        for (size_t j = 0; j < s_history_count; j++) {
            uint8_t other = s_gate_history[j];
            if (other == candidate || other + 1 == candidate || other == candidate + 1) {
                count++;
            }
        }
        if (count > best_count) {
            best_count = count;
            best_gate = candidate;
        }
    }
    if (same_count) {
        *same_count = best_count;
    }
    return best_gate;
}

static uint8_t radar_history_at(const uint8_t *history, size_t age_from_oldest)
{
    if (s_history_count == 0 || age_from_oldest >= s_history_count) {
        return 0;
    }

    size_t oldest = s_history_count < FRIDGE_RADAR_HISTORY_COUNT ? 0 : s_history_index;
    return history[(oldest + age_from_oldest) % FRIDGE_RADAR_HISTORY_COUNT];
}

static uint16_t radar_u16_history_at(const uint16_t *history, size_t age_from_oldest)
{
    if (s_history_count == 0 || age_from_oldest >= s_history_count) {
        return 0;
    }

    size_t oldest = s_history_count < FRIDGE_RADAR_HISTORY_COUNT ? 0 : s_history_index;
    return history[(oldest + age_from_oldest) % FRIDGE_RADAR_HISTORY_COUNT];
}

static uint8_t radar_calculate_approach_score(void)
{
    if (s_history_count < FRIDGE_RADAR_APPROACH_MIN_SAMPLES) {
        return 0;
    }

    uint32_t score = 0;
    uint8_t first = radar_history_at(s_approach_history, 0);
    uint8_t last = radar_history_at(s_approach_history, s_history_count - 1);
    uint8_t falling_steps = 0;

    for (size_t i = 1; i < s_history_count; i++) {
        uint8_t previous = radar_history_at(s_approach_history, i - 1);
        uint8_t current = radar_history_at(s_approach_history, i);
        if (current < previous) {
            falling_steps++;
        }
    }

    if (first > last) {
        score = (uint32_t)(first - last) * 35U;
    }
    score += (uint32_t)falling_steps * 15U;
    if (score > 100) {
        score = 100;
    }
    return (uint8_t)score;
}

// 统计最近窗口内距离是否持续变小。单次门位跳变可能来自墙面多径，连续下降才更像人靠近。
static void radar_calculate_approach_trend(uint8_t *approach_frames, uint16_t *distance_delta)
{
    if (approach_frames) {
        *approach_frames = 0;
    }
    if (distance_delta) {
        *distance_delta = 0;
    }
    if (s_history_count < FRIDGE_RADAR_APPROACH_MIN_SAMPLES) {
        return;
    }

    uint8_t falling_frames = 0;
    uint16_t first = radar_u16_history_at(s_distance_history, 0);
    uint16_t last = radar_u16_history_at(s_distance_history, s_history_count - 1);

    for (size_t i = 1; i < s_history_count; i++) {
        uint16_t previous = radar_u16_history_at(s_distance_history, i - 1);
        uint16_t current = radar_u16_history_at(s_distance_history, i);
        if (previous > current && (previous - current) >= 8) {
            falling_frames++;
        }
    }

    if (approach_frames) {
        *approach_frames = falling_frames;
    }
    if (distance_delta && first > last) {
        *distance_delta = (uint16_t)(first - last);
    }
}

static uint8_t radar_clamp_u8(uint32_t value)
{
    return value > 100 ? 100 : (uint8_t)value;
}

static uint16_t radar_abs_u16(uint16_t a, uint16_t b)
{
    return a > b ? (uint16_t)(a - b) : (uint16_t)(b - a);
}

// 计算最近几帧的“微动证据”。
// 墙壁/柜门这类静态反射通常门位、距离和能量都很稳定；人体即使站立也会带来轻微距离或能量抖动。
// 这里不新增浮点和外部依赖，只用整数差分估算，便于在 ESP32-S3 上持续运行。
static void radar_calculate_motion_features(uint8_t *motion_score,
                                             uint8_t *distance_span,
                                             uint8_t *gate_span,
                                             uint8_t *energy_change_score)
{
    if (motion_score) {
        *motion_score = 0;
    }
    if (distance_span) {
        *distance_span = 0;
    }
    if (gate_span) {
        *gate_span = 0;
    }
    if (energy_change_score) {
        *energy_change_score = 0;
    }
    if (s_history_count < 2) {
        return;
    }

    uint16_t min_distance = UINT16_MAX;
    uint16_t max_distance = 0;
    uint8_t min_gate = UINT8_MAX;
    uint8_t max_gate = 0;
    uint32_t energy_delta_sum = 0;

    for (size_t i = 0; i < s_history_count; i++) {
        uint16_t distance = radar_u16_history_at(s_distance_history, i);
        uint8_t gate = radar_history_at(s_gate_history, i);
        if (distance < min_distance) {
            min_distance = distance;
        }
        if (distance > max_distance) {
            max_distance = distance;
        }
        if (gate < min_gate) {
            min_gate = gate;
        }
        if (gate > max_gate) {
            max_gate = gate;
        }
        if (i > 0) {
            energy_delta_sum += radar_abs_u16(radar_u16_history_at(s_energy_history, i),
                                              radar_u16_history_at(s_energy_history, i - 1));
        }
    }

    uint8_t local_distance_span = max_distance >= min_distance ? radar_clamp_u8(max_distance - min_distance) : 0;
    uint8_t local_gate_span = max_gate >= min_gate ? (uint8_t)(max_gate - min_gate) : 0;
    uint8_t local_energy_change = radar_clamp_u8(energy_delta_sum / FRIDGE_RADAR_ENERGY_CHANGE_UNIT);
    uint32_t score = ((uint32_t)local_distance_span * 3U / FRIDGE_RADAR_DISTANCE_JITTER_UNIT) +
                     ((uint32_t)local_gate_span * 22U) +
                     local_energy_change;

    if (motion_score) {
        *motion_score = radar_clamp_u8(score);
    }
    if (distance_span) {
        *distance_span = local_distance_span;
    }
    if (gate_span) {
        *gate_span = local_gate_span;
    }
    if (energy_change_score) {
        *energy_change_score = local_energy_change;
    }
}

// 计算静态反射分数。分数越高，越像墙面、柜门、金属边框这类强而稳定的背景反射。
// 成熟上位机通常只把它显示为静止目标/门能量，不直接宣称“可靠人体”；这里用于抑制误报。
static uint8_t radar_calculate_static_score(uint8_t stability,
                                            uint8_t motion_score,
                                            uint8_t distance_span,
                                            uint8_t gate_span,
                                            uint8_t energy_change_score,
                                            bool threshold_frame,
                                            bool holdover_frame)
{
    uint32_t score = 0;

    if (stability > 55) {
        score += (uint32_t)(stability - 55U) * 2U;
    }
    if (motion_score < 40) {
        score += (uint32_t)(40U - motion_score);
    }
    if (distance_span <= 10) {
        score += 18;
    }
    if (gate_span == 0) {
        score += 18;
    } else if (gate_span == 1) {
        score += 8;
    }
    if (energy_change_score <= 12) {
        score += 16;
    }
    if (!threshold_frame && holdover_frame) {
        score += 10;
    }

    return radar_clamp_u8(score);
}

// 计算人体候选分数。只有阈值命中、微动/靠近证据和稳定历史共同满足时，才升级为可靠人体。
// 这样对着墙时即使门位稳定、能量很高，也只会停留在静态反射或原始目标层。
static uint8_t radar_calculate_human_score(uint8_t threshold_score,
                                           uint8_t dominance,
                                           uint8_t stability,
                                           uint8_t motion_score,
                                           uint8_t approach_score,
                                           bool threshold_frame,
                                           bool holdover_frame)
{
    uint32_t score = 0;

    if (threshold_frame) {
        score += 20;
    } else if (holdover_frame) {
        score += 8;
    }
    score += (uint32_t)threshold_score * 22U / 100U;
    score += (uint32_t)(dominance > 50 ? 50 : dominance) * 16U / 50U;
    score += (uint32_t)stability * 12U / 100U;
    score += (uint32_t)(motion_score > 50 ? 50 : motion_score) * 32U / 50U;
    score += (uint32_t)(approach_score > 70 ? 70 : approach_score) * 18U / 70U;

    return radar_clamp_u8(score);
}

static bool radar_gate_within_operator_range(size_t gate)
{
    return gate < FRIDGE_RADAR_GATE_COUNT && gate <= FRIDGE_RADAR_OPERATOR_GATE_MAX;
}

static bool radar_gate_passes_threshold(uint16_t energy, size_t gate, bool holding)
{
    if (!radar_gate_within_operator_range(gate)) {
        return false;
    }
    uint16_t threshold = holding ? s_hold_threshold[gate] : s_trigger_threshold[gate];
    return energy >= threshold;
}

static uint8_t radar_gate_threshold_score(uint16_t energy, size_t gate, bool holding)
{
    if (!radar_gate_within_operator_range(gate)) {
        return 0;
    }
    uint16_t threshold = holding ? s_hold_threshold[gate] : s_trigger_threshold[gate];
    if (threshold == 0 || energy < threshold) {
        return 0;
    }
    uint32_t score = ((uint32_t)energy - threshold) * 100U / threshold;
    if (score > 100) {
        score = 100;
    }
    return (uint8_t)score;
}

static void radar_update_stable_estimate(fridge_radar_snapshot_t *next)
{
    uint32_t total = 0;
    uint32_t near = 0;
    uint32_t mid = 0;
    uint32_t far = 0;
    uint16_t peak_energy = 0;
    uint16_t remote_peak_energy = 0;
    uint16_t threshold_peak_energy = 0;
    uint8_t peak_gate = 0;
    uint8_t remote_peak_gate = 0;
    uint8_t threshold_peak_gate = 0;
    uint8_t threshold_score = 0;

    for (size_t i = 0; i < FRIDGE_RADAR_GATE_COUNT; i++) {
        uint16_t energy = next->gate_energy[i];
        total += energy;
        if (i <= 3) {
            near += energy;
        } else if (i <= 8) {
            mid += energy;
        } else {
            far += energy;
        }
        if (energy > peak_energy) {
            peak_energy = energy;
            peak_gate = (uint8_t)i;
        }
        if (i >= FRIDGE_RADAR_REMOTE_GATE_MIN && energy > remote_peak_energy) {
            remote_peak_energy = energy;
            remote_peak_gate = (uint8_t)i;
        }
        if (radar_gate_passes_threshold(energy, i, s_threshold_presence)) {
            uint8_t score = radar_gate_threshold_score(energy, i, s_threshold_presence);
            if (score > threshold_score || (score == threshold_score && energy > threshold_peak_energy)) {
                threshold_score = score;
                threshold_peak_energy = energy;
                threshold_peak_gate = (uint8_t)i;
            }
        }
    }

    next->peak_gate = peak_gate;
    next->peak_energy = peak_energy;
    next->near_energy = radar_u32_to_u16(near);
    next->mid_energy = radar_u32_to_u16(mid);
    next->far_energy = radar_u32_to_u16(far);

    uint8_t estimated_gate = threshold_score > 0 ? threshold_peak_gate : peak_gate;
    uint16_t estimated_energy = threshold_score > 0 ? threshold_peak_energy : peak_energy;
    if (peak_gate <= FRIDGE_RADAR_NEAR_CLUTTER_GATE_MAX && remote_peak_energy >= FRIDGE_RADAR_MIN_PEAK_ENERGY) {
        uint32_t remote_ratio = peak_energy > 0 ? ((uint32_t)remote_peak_energy * 100U / peak_energy) : 0;
        uint8_t raw_gate = radar_gate_from_distance_raw(next->distance_raw);
        if (remote_ratio >= FRIDGE_RADAR_REMOTE_PEAK_RATIO_PERCENT &&
            raw_gate >= FRIDGE_RADAR_REMOTE_GATE_MIN &&
            radar_gate_passes_threshold(remote_peak_energy, remote_peak_gate, s_threshold_presence)) {
            estimated_gate = remote_peak_gate;
            estimated_energy = remote_peak_energy;
        }
    }
    next->estimated_gate = estimated_gate;
    next->threshold_gate = threshold_peak_gate;
    next->threshold_score = threshold_score;
    next->threshold_presence = s_threshold_presence;
    next->hold_frames_remaining = s_disappear_frames_remaining;

    bool enough_energy = estimated_energy >= FRIDGE_RADAR_MIN_PEAK_ENERGY;
    uint8_t dominance = total > 0 ? (uint8_t)((uint32_t)peak_energy * 100U / total) : 0;
    uint8_t remote_ratio = peak_energy > 0 ? (uint8_t)((uint32_t)remote_peak_energy * 100U / peak_energy) : 0;
    bool threshold_frame = threshold_score > 0;
    bool near_clutter = next->presence &&
                        peak_gate <= FRIDGE_RADAR_NEAR_CLUTTER_GATE_MAX &&
                        next->distance_raw > 0 &&
                        next->distance_raw <= FRIDGE_RADAR_NEAR_CLUTTER_DISTANCE_RAW_MAX &&
                        remote_ratio <= FRIDGE_RADAR_NEAR_CLUTTER_REMOTE_RATIO_MAX;
    bool usable_frame = next->presence && !near_clutter && threshold_frame && enough_energy && dominance >= FRIDGE_RADAR_MIN_DOMINANCE_PERCENT;
    bool holdover_frame = false;
    next->near_clutter = near_clutter;
    next->static_clutter = false;

    if (usable_frame) {
        s_threshold_presence = true;
        s_disappear_frames_remaining = FRIDGE_RADAR_DISAPPEAR_DELAY_FRAMES;
        s_gate_history[s_history_index] = estimated_gate;
        s_distance_history[s_history_index] = next->distance_raw;
        s_energy_history[s_history_index] = estimated_energy;
        s_approach_history[s_history_index] = estimated_gate;
        s_history_index = (s_history_index + 1) % FRIDGE_RADAR_HISTORY_COUNT;
        if (s_history_count < FRIDGE_RADAR_HISTORY_COUNT) {
            s_history_count++;
        }
    } else if (s_threshold_presence && s_disappear_frames_remaining > 0 && !near_clutter) {
        holdover_frame = true;
        s_disappear_frames_remaining--;
    } else if (!next->presence || near_clutter || !threshold_frame) {
        s_threshold_presence = false;
        s_disappear_frames_remaining = 0;
        radar_reset_filter();
    }
    next->threshold_presence = s_threshold_presence;
    next->hold_frames_remaining = s_disappear_frames_remaining;

    uint8_t same_count = 0;
    uint8_t stable_gate = radar_most_common_gate(estimated_gate, &same_count);
    uint8_t stability = s_history_count > 0 ? (uint8_t)((uint32_t)same_count * 100U / s_history_count) : 0;
    uint8_t motion_score = 0;
    uint8_t distance_span = 0;
    uint8_t gate_span = 0;
    uint8_t energy_change_score = 0;
    uint8_t confidence = 0;
    radar_calculate_motion_features(&motion_score, &distance_span, &gate_span, &energy_change_score);

    uint8_t approach_score = radar_calculate_approach_score();
    uint8_t approach_frames = 0;
    uint16_t approach_distance_delta = 0;
    radar_calculate_approach_trend(&approach_frames, &approach_distance_delta);
    uint8_t static_score = radar_calculate_static_score(stability,
                                                        motion_score,
                                                        distance_span,
                                                        gate_span,
                                                        energy_change_score,
                                                        threshold_frame,
                                                        holdover_frame);
    uint8_t human_score = radar_calculate_human_score(threshold_score,
                                                      dominance,
                                                      stability,
                                                      motion_score,
                                                      approach_score,
                                                      threshold_frame,
                                                      holdover_frame);
    bool motion_confirmed = motion_score >= FRIDGE_RADAR_MOTION_MIN_SCORE ||
                            approach_score >= 60 ||
                            (distance_span >= 18 && energy_change_score >= 12);
    bool static_clutter = (usable_frame || holdover_frame) &&
                          s_history_count >= FRIDGE_RADAR_STATIC_CLUTTER_HISTORY_MIN &&
                          (stability >= FRIDGE_RADAR_STATIC_CLUTTER_STABILITY_MIN ||
                           static_score >= FRIDGE_RADAR_STATIC_SCORE_BLOCK_MIN) &&
                          (motion_score <= FRIDGE_RADAR_STATIC_CLUTTER_MOTION_MAX ||
                           static_score >= FRIDGE_RADAR_STATIC_SCORE_BLOCK_MIN) &&
                          !motion_confirmed;
    next->static_clutter = static_clutter;
    next->human_candidate = false;
    next->motion_score = motion_score;
    next->distance_span = distance_span;
    next->gate_span = gate_span;
    next->energy_change_score = energy_change_score;
    next->static_score = static_score;
    next->human_score = human_score;

    if (usable_frame) {
        uint8_t energy_score = (uint8_t)((uint32_t)threshold_score * 45U / 100U);
        uint8_t dominance_score = dominance > 55 ? 25 : (uint8_t)((uint32_t)dominance * 25U / 55U);
        uint8_t stability_score = (uint8_t)((uint32_t)stability * 20U / 100U);
        uint8_t motion_confirm_score = motion_score > 35 ? 10 : (uint8_t)((uint32_t)motion_score * 10U / 35U);
        confidence = energy_score + dominance_score + stability_score + motion_confirm_score;
        if (confidence > 100) {
            confidence = 100;
        }
    } else if (holdover_frame && s_history_count > 0) {
        uint8_t stability_score = (uint8_t)((uint32_t)stability * 30U / 100U);
        confidence = stability_score > 70 ? 70 : stability_score;
    }

    if (static_clutter) {
        confidence = confidence > 55 ? 55 : confidence;
    }

    next->human_candidate = (usable_frame || holdover_frame) &&
                            !near_clutter &&
                            !static_clutter &&
                            same_count >= FRIDGE_RADAR_STABLE_REQUIRED &&
                            motion_score >= FRIDGE_RADAR_STABLE_MIN_MOTION_SCORE &&
                            human_score >= FRIDGE_RADAR_HUMAN_CANDIDATE_MIN_SCORE;
    next->stable_presence = next->human_candidate &&
                            (human_score >= FRIDGE_RADAR_RELIABLE_HUMAN_MIN_SCORE ||
                             (approach_score >= 60 &&
                              approach_frames >= FRIDGE_RADAR_APPROACH_MIN_FRAMES &&
                              approach_distance_delta >= FRIDGE_RADAR_APPROACH_MIN_DISTANCE_DELTA));
    next->stable_gate = next->stable_presence ? stable_gate : estimated_gate;
    next->stable_zone = next->stable_presence ? radar_zone_from_gate(next->stable_gate) : FRIDGE_RADAR_DISTANCE_UNKNOWN;
    next->confidence = confidence;
    next->stability = stability;
    next->approach_score = approach_score;
    next->approach_frames = approach_frames;
    next->approach_distance_delta = approach_distance_delta;
    next->approaching = next->stable_presence &&
                        !next->near_clutter &&
                        next->confidence >= FRIDGE_RADAR_APPROACH_MIN_CONFIDENCE &&
                        next->approach_score >= 60 &&
                        next->approach_frames >= FRIDGE_RADAR_APPROACH_MIN_FRAMES &&
                        next->approach_distance_delta >= FRIDGE_RADAR_APPROACH_MIN_DISTANCE_DELTA;
    next->smoothed_distance_raw = radar_average_distance();
    next->within_1m = next->stable_presence &&
                      !next->near_clutter &&
                      next->stable_gate <= FRIDGE_RADAR_WITHIN_1M_GATE_MAX &&
                      next->smoothed_distance_raw > 0 &&
                      next->smoothed_distance_raw <= FRIDGE_RADAR_WITHIN_1M_DISTANCE_RAW_MAX &&
                      next->confidence >= FRIDGE_RADAR_WITHIN_1M_MIN_CONFIDENCE;

    if (next->approaching) {
        strlcpy(next->target_class, "reliable_approaching", sizeof(next->target_class));
        next->rejection_reason[0] = '\0';
        snprintf(next->last_text, sizeof(next->last_text), "approaching gate=%u score=%u",
                 (unsigned)next->stable_gate,
                 (unsigned)next->approach_score);
    } else if (next->within_1m) {
        strlcpy(next->target_class, "reliable_within_1m", sizeof(next->target_class));
        next->rejection_reason[0] = '\0';
        snprintf(next->last_text, sizeof(next->last_text), "likely within 1m gate=%u confidence=%u",
                 (unsigned)next->stable_gate,
                 (unsigned)next->confidence);
    } else if (next->stable_presence) {
        strlcpy(next->target_class, "reliable_human", sizeof(next->target_class));
        next->rejection_reason[0] = '\0';
        snprintf(next->last_text, sizeof(next->last_text), "%s gate=%u confidence=%u",
                 radar_zone_text(next->stable_zone),
                 (unsigned)next->stable_gate,
                 (unsigned)next->confidence);
    } else if (next->human_candidate) {
        strlcpy(next->target_class, "human_candidate", sizeof(next->target_class));
        strlcpy(next->rejection_reason, "motion/history not strong enough", sizeof(next->rejection_reason));
        snprintf(next->last_text, sizeof(next->last_text), "candidate gate=%u human=%u motion=%u",
                 (unsigned)estimated_gate,
                 (unsigned)next->human_score,
                 (unsigned)next->motion_score);
    } else if (next->static_clutter) {
        strlcpy(next->target_class, "static_reflection", sizeof(next->target_class));
        strlcpy(next->rejection_reason, "stable low-motion reflection", sizeof(next->rejection_reason));
        snprintf(next->last_text, sizeof(next->last_text), "static clutter gate=%u motion=%u",
                 (unsigned)estimated_gate,
                 (unsigned)next->motion_score);
    } else if (near_clutter) {
        strlcpy(next->target_class, "near_clutter", sizeof(next->target_class));
        strlcpy(next->rejection_reason, "too close to sensor face", sizeof(next->rejection_reason));
        strlcpy(next->last_text, "near clutter", sizeof(next->last_text));
    } else if (next->presence) {
        strlcpy(next->target_class, "raw_target", sizeof(next->target_class));
        if (!threshold_frame) {
            strlcpy(next->rejection_reason, "below vendor threshold", sizeof(next->rejection_reason));
        } else if (motion_score < FRIDGE_RADAR_STABLE_MIN_MOTION_SCORE) {
            strlcpy(next->rejection_reason, "micro-motion too weak", sizeof(next->rejection_reason));
        } else {
            strlcpy(next->rejection_reason, "waiting for stable history", sizeof(next->rejection_reason));
        }
        strlcpy(next->last_text, "unstable target", sizeof(next->last_text));
    } else {
        strlcpy(next->target_class, "idle", sizeof(next->target_class));
        strlcpy(next->rejection_reason, "no raw target", sizeof(next->rejection_reason));
        strlcpy(next->last_text, "idle", sizeof(next->last_text));
    }
}

static void radar_snapshot_assign(const fridge_radar_snapshot_t *next)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot = *next;
    xSemaphoreGive(s_lock);
}

static void radar_snapshot_touch_error(const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot.ready = s_uart_ready;
    s_snapshot.mode = FRIDGE_RADAR_MODE_ERROR;
    s_snapshot.parse_error_count++;
    s_snapshot.updated_at_ms = esp_timer_get_time() / 1000;
    strlcpy(s_snapshot.last_error, message ? message : "", sizeof(s_snapshot.last_error));
    xSemaphoreGive(s_lock);
}

static void radar_snapshot_touch_text(const char *text)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot.ready = s_uart_ready;
    s_snapshot.updated_at_ms = esp_timer_get_time() / 1000;
    if (text) {
        strlcpy(s_snapshot.last_text, text, sizeof(s_snapshot.last_text));
    } else {
        s_snapshot.last_text[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

static void radar_snapshot_touch_frame_error(const char *message, const uint8_t *frame, size_t len)
{
    char hex[128] = {0};
    size_t limit = len < 48 ? len : 48;
    size_t written = 0;
    for (size_t i = 0; i < limit && written + 3 < sizeof(hex); i++) {
        written += (size_t)snprintf(hex + written, sizeof(hex) - written, "%02X", frame[i]);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot.ready = s_uart_ready;
    s_snapshot.parse_error_count++;
    s_snapshot.updated_at_ms = esp_timer_get_time() / 1000;
    strlcpy(s_snapshot.last_error, message ? message : "", sizeof(s_snapshot.last_error));
    strlcat(s_snapshot.last_error, " len=", sizeof(s_snapshot.last_error));
    char len_buf[16];
    snprintf(len_buf, sizeof(len_buf), "%u", (unsigned)len);
    strlcat(s_snapshot.last_error, len_buf, sizeof(s_snapshot.last_error));
    strlcat(s_snapshot.last_error, " raw=", sizeof(s_snapshot.last_error));
    strlcat(s_snapshot.last_error, hex, sizeof(s_snapshot.last_error));
    xSemaphoreGive(s_lock);
}

static void radar_reset_snapshot(fridge_radar_mode_t mode)
{
    radar_reset_filter();
    fridge_radar_snapshot_t next = {0};
    next.ready = true;
    next.mode = mode;
    next.ot2_level = -1;
    next.updated_at_ms = esp_timer_get_time() / 1000;
    strlcpy(next.last_text, mode == FRIDGE_RADAR_MODE_REPORT ? "report mode" : "normal mode", sizeof(next.last_text));
    radar_snapshot_assign(&next);
}

static esp_err_t radar_send_mode_command(const uint8_t *cmd, size_t len, fridge_radar_mode_t mode)
{
    if (!s_uart_ready) {
        radar_snapshot_touch_error("radar uart not ready");
        return ESP_ERR_INVALID_STATE;
    }

    int written = uart_write_bytes(FRIDGE_RADAR_UART, (const char *)cmd, len);
    if (written != (int)len) {
        radar_snapshot_touch_error("radar mode command write failed");
        ESP_LOGW(TAG, "mode command write incomplete: %d/%u", written, (unsigned)len);
        return ESP_FAIL;
    }

    uart_wait_tx_done(FRIDGE_RADAR_UART, pdMS_TO_TICKS(100));
    radar_reset_snapshot(mode);
    ESP_LOGI(TAG, "radar mode switched to %s", mode == FRIDGE_RADAR_MODE_REPORT ? "report" : "normal");
    return ESP_OK;
}

static esp_err_t radar_send_report_mode(void)
{
    return radar_send_mode_command(s_report_mode_cmd, sizeof(s_report_mode_cmd), FRIDGE_RADAR_MODE_REPORT);
}

static esp_err_t radar_send_normal_mode(void)
{
    return radar_send_mode_command(s_normal_mode_cmd, sizeof(s_normal_mode_cmd), FRIDGE_RADAR_MODE_NORMAL);
}

static size_t radar_find_byte(const uint8_t *buffer, size_t len, uint8_t value)
{
    for (size_t i = 0; i < len; i++) {
        if (buffer[i] == value) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t radar_find_header(const uint8_t *buffer, size_t len)
{
    static const uint8_t header[] = {0xF4, 0xF3, 0xF2, 0xF1};
    if (len < sizeof(header)) {
        return SIZE_MAX;
    }
    for (size_t i = 0; i + sizeof(header) <= len; i++) {
        if (memcmp(&buffer[i], header, sizeof(header)) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static bool radar_line_is_printable(const char *line, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char ch = line[i];
        if (ch == '\r' || ch == '\n') {
            continue;
        }
        if (!isprint((unsigned char)ch) && !isspace((unsigned char)ch)) {
            return false;
        }
    }
    return true;
}

static void radar_parse_ascii_line(const uint8_t *line, size_t len)
{
    char text[64] = {0};
    if (len >= sizeof(text)) {
        len = sizeof(text) - 1;
    }
    memcpy(text, line, len);
    text[len] = '\0';

    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    radar_snapshot_touch_text(text);

    if (strcasecmp(text, "ON") == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_snapshot.presence = true;
        s_snapshot.ready = s_uart_ready;
        s_snapshot.mode = s_snapshot.mode == FRIDGE_RADAR_MODE_IDLE ? FRIDGE_RADAR_MODE_NORMAL : s_snapshot.mode;
        s_snapshot.updated_at_ms = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_lock);
        return;
    }

    if (strcasecmp(text, "OFF") == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_snapshot.presence = false;
        s_snapshot.ready = s_uart_ready;
        s_snapshot.mode = s_snapshot.mode == FRIDGE_RADAR_MODE_IDLE ? FRIDGE_RADAR_MODE_NORMAL : s_snapshot.mode;
        s_snapshot.updated_at_ms = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_lock);
        return;
    }

    if (strncasecmp(text, "Range", 5) == 0) {
        const char *number = text + 5;
        while (*number && !isdigit((unsigned char)*number)) {
            number++;
        }
        if (*number) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_snapshot.distance_raw = (uint16_t)strtoul(number, NULL, 10);
            s_snapshot.presence = true;
            s_snapshot.ready = s_uart_ready;
            s_snapshot.mode = s_snapshot.mode == FRIDGE_RADAR_MODE_IDLE ? FRIDGE_RADAR_MODE_NORMAL : s_snapshot.mode;
            s_snapshot.updated_at_ms = esp_timer_get_time() / 1000;
            xSemaphoreGive(s_lock);
        }
        return;
    }
}

static void radar_parse_frame(const uint8_t *frame, size_t len)
{
    if (len < FRIDGE_RADAR_MIN_FRAME_LEN) {
        radar_snapshot_touch_error("radar frame size mismatch");
        return;
    }
    uint16_t payload_len = (uint16_t)frame[4] | ((uint16_t)frame[5] << 8);
    size_t expected_len = 4 + 2 + (size_t)payload_len + 4;
    if (payload_len < 3 || expected_len != len || expected_len > FRIDGE_RADAR_MAX_FRAME_LEN) {
        radar_snapshot_touch_frame_error("radar payload length mismatch", frame, len);
        return;
    }

    bool tail_ok = false;
    for (size_t i = 0; i < sizeof(s_report_tail_candidates) / sizeof(s_report_tail_candidates[0]); i++) {
        if (memcmp(&frame[len - 4], s_report_tail_candidates[i], 4) == 0) {
            tail_ok = true;
            break;
        }
    }
    if (!tail_ok) {
        radar_snapshot_touch_frame_error("radar tail mismatch", frame, len);
        return;
    }

    fridge_radar_snapshot_t next = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    next = s_snapshot;
    xSemaphoreGive(s_lock);

    next.ready = true;
    next.mode = FRIDGE_RADAR_MODE_REPORT;
    next.presence = frame[6] != 0;
    next.distance_raw = (uint16_t)frame[7] | ((uint16_t)frame[8] << 8);
    memset(next.gate_energy, 0, sizeof(next.gate_energy));
    size_t gate_bytes = payload_len > 3 ? (size_t)payload_len - 3 : 0;
    size_t gate_count = gate_bytes / 2;
    if (gate_count > FRIDGE_RADAR_GATE_COUNT) {
        gate_count = FRIDGE_RADAR_GATE_COUNT;
    }
    for (size_t i = 0; i < gate_count; i++) {
        size_t offset = 9 + (i * 2);
        next.gate_energy[i] = (uint16_t)frame[offset] | ((uint16_t)frame[offset + 1] << 8);
    }
    next.frame_count++;
    next.updated_at_ms = esp_timer_get_time() / 1000;
    next.timeout_count = s_snapshot.timeout_count;
    next.parse_error_count = s_snapshot.parse_error_count;
    next.ot2_level = -1;
    next.last_error[0] = '\0';
    radar_update_stable_estimate(&next);
    radar_snapshot_assign(&next);
}

static void radar_consume_buffer(void)
{
    while (s_rx_len > 0) {
        size_t header_pos = radar_find_header((const uint8_t *)s_rx_buffer, s_rx_len);
        if (header_pos != SIZE_MAX) {
            if (header_pos > 0) {
                memmove(s_rx_buffer, s_rx_buffer + header_pos, s_rx_len - header_pos);
                s_rx_len -= header_pos;
                continue;
            }
            if (s_rx_len < 6) {
                return;
            }
            uint16_t payload_len = (uint16_t)(uint8_t)s_rx_buffer[4] | ((uint16_t)(uint8_t)s_rx_buffer[5] << 8);
            size_t frame_len = 4 + 2 + (size_t)payload_len + 4;
            if (payload_len < 3 || frame_len > FRIDGE_RADAR_MAX_FRAME_LEN) {
                radar_snapshot_touch_frame_error("radar invalid frame length", (const uint8_t *)s_rx_buffer, s_rx_len < 24 ? s_rx_len : 24);
                memmove(s_rx_buffer, s_rx_buffer + 1, s_rx_len - 1);
                s_rx_len--;
                continue;
            }
            if (s_rx_len < frame_len) {
                return;
            }
            radar_parse_frame((const uint8_t *)s_rx_buffer, frame_len);
            memmove(s_rx_buffer, s_rx_buffer + frame_len, s_rx_len - frame_len);
            s_rx_len -= frame_len;
            continue;
        }

        size_t newline_pos = radar_find_byte((const uint8_t *)s_rx_buffer, s_rx_len, '\n');
        if (newline_pos != SIZE_MAX) {
            size_t line_len = newline_pos;
            if (line_len > 0 && s_rx_buffer[line_len - 1] == '\r') {
                line_len--;
            }
            if (line_len > 0 && radar_line_is_printable(s_rx_buffer, line_len)) {
                radar_parse_ascii_line((const uint8_t *)s_rx_buffer, line_len);
            }
            memmove(s_rx_buffer, s_rx_buffer + newline_pos + 1, s_rx_len - newline_pos - 1);
            s_rx_len -= newline_pos + 1;
            continue;
        }

        if (s_rx_len >= FRIDGE_RADAR_RX_BUFFER - 8) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_snapshot.parse_error_count++;
            xSemaphoreGive(s_lock);
            s_rx_len = 0;
        }
        return;
    }
}

static void radar_task(void *arg)
{
    (void)arg;
    uint8_t chunk[FRIDGE_RADAR_READ_CHUNK] = {0};

    while (true) {
        int read = uart_read_bytes(FRIDGE_RADAR_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        if (read > 0) {
            size_t copy = (size_t)read;
            if (copy > sizeof(s_rx_buffer) - s_rx_len) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_snapshot.parse_error_count++;
                xSemaphoreGive(s_lock);
                s_rx_len = 0;
            }
            if (copy <= sizeof(s_rx_buffer) - s_rx_len) {
                memcpy(s_rx_buffer + s_rx_len, chunk, copy);
                s_rx_len += copy;
                radar_consume_buffer();
            }
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_snapshot.timeout_count++;
            s_snapshot.updated_at_ms = esp_timer_get_time() / 1000;
            xSemaphoreGive(s_lock);
        }
    }
}

esp_err_t fridge_radar_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    uart_config_t config = {
        .baud_rate = FRIDGE_RADAR_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(FRIDGE_RADAR_UART, &config);
    if (err != ESP_OK) {
        radar_snapshot_touch_error("radar uart config failed");
        return err;
    }

    err = uart_set_pin(FRIDGE_RADAR_UART, FRIDGE_RADAR_UART_TX_GPIO, FRIDGE_RADAR_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        radar_snapshot_touch_error("radar uart pin config failed");
        return err;
    }

    err = uart_driver_install(FRIDGE_RADAR_UART, 1024, 1024, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        radar_snapshot_touch_error("radar uart driver install failed");
        return err;
    }

    // N8R8 安全排线中雷达 OT2 不接，GPIO19 让给 OV3660 PCLK。

    s_uart_ready = true;
    radar_reset_snapshot(FRIDGE_RADAR_MODE_NORMAL);
    uart_flush_input(FRIDGE_RADAR_UART);
    err = radar_send_normal_mode();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "normal mode command failed: %s", esp_err_to_name(err));
    }

    BaseType_t ok = xTaskCreate(radar_task, "radar_task", FRIDGE_RADAR_TASK_STACK, NULL, 5, NULL);
    if (ok != pdPASS) {
        radar_snapshot_touch_error("radar task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "radar ready on UART1 RX=GPIO%d TX=GPIO%d baud=%d", FRIDGE_RADAR_UART_RX_GPIO, FRIDGE_RADAR_UART_TX_GPIO, FRIDGE_RADAR_UART_BAUDRATE);
    return ESP_OK;
}

esp_err_t fridge_radar_start_report_mode(void)
{
    esp_err_t err = fridge_radar_init();
    if (err != ESP_OK) {
        return err;
    }
    return radar_send_report_mode();
}

esp_err_t fridge_radar_start_normal_mode(void)
{
    esp_err_t err = fridge_radar_init();
    if (err != ESP_OK) {
        return err;
    }
    return radar_send_normal_mode();
}

esp_err_t fridge_radar_get_snapshot(fridge_radar_snapshot_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        out->mode = FRIDGE_RADAR_MODE_IDLE;
        out->ot2_level = -1;
        strlcpy(out->last_error, "radar not initialized", sizeof(out->last_error));
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
