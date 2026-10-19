#pragma once

#include <math.h>
#include <stdint.h>

struct LevelProjection {
    float x;
    float y;
    float tilt_deg;
};

constexpr int LEVEL_CAL_SAMPLES = 64;
constexpr float LEVEL_VECTOR_DEADZONE = 0.10f;

struct LevelCalibration {
    float sum_x;
    float sum_y;
    int count;
    bool active;
};

inline LevelProjection level_project(float panel_x, float panel_y,
                                     uint8_t rotation, float max_offset) {
    float screen_x = panel_x;
    float screen_y = panel_y;
    switch (rotation & 3U) {
    case 1: screen_x =  panel_y; screen_y = -panel_x; break;
    case 2: screen_x = -panel_x; screen_y = -panel_y; break;
    case 3: screen_x = -panel_y; screen_y =  panel_x; break;
    default: break;
    }

    const float magnitude = sqrtf(screen_x * screen_x + screen_y * screen_y);
    const float unit_magnitude = magnitude > 1.0f ? 1.0f : magnitude;
    const float tilt_deg = asinf(unit_magnitude) * 57.2957795f;

    LevelProjection result = {0.0f, 0.0f, tilt_deg};
    if (magnitude <= LEVEL_VECTOR_DEADZONE || max_offset <= 0.0f) return result;

    // Level is a gravity ball, not a bubble whose travel is proportional to
    // degrees: any measurable tilt pulls it to the low edge. The deadzone is
    // applied to sensor samples before they reach this model.
    result.x = screen_x / magnitude * max_offset;
    result.y = screen_y / magnitude * max_offset;
    return result;
}

inline LevelProjection level_project_sensor(float sensor_x, float sensor_y,
                                            uint8_t rotation, float max_offset) {
    // This board's QMI8658 X/Y axes are exchanged relative to the panel.
    // Sensor signs are already aligned with the board's physical low-side
    // directions; retain them when mapping into panel space.
    return level_project(sensor_y, sensor_x, rotation, max_offset);
}

inline void level_calibration_begin(LevelCalibration* cal) {
    if (!cal) return;
    cal->sum_x = 0.0f;
    cal->sum_y = 0.0f;
    cal->count = 0;
    cal->active = true;
}

inline bool level_calibration_add(LevelCalibration* cal, float x, float y,
                                  float* bias_x, float* bias_y, int* pct_out) {
    if (!cal || !cal->active) {
        if (pct_out) *pct_out = 0;
        return false;
    }

    cal->sum_x += x;
    cal->sum_y += y;
    ++cal->count;

    int pct = cal->count * 100 / LEVEL_CAL_SAMPLES;
    if (pct > 100) pct = 100;
    if (pct_out) *pct_out = pct;
    if (cal->count < LEVEL_CAL_SAMPLES) return false;

    cal->active = false;
    if (bias_x) *bias_x = cal->sum_x / (float)LEVEL_CAL_SAMPLES;
    if (bias_y) *bias_y = cal->sum_y / (float)LEVEL_CAL_SAMPLES;
    return true;
}
