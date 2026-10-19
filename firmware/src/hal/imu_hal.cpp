#include "imu_hal.h"
#include "../apps/level/level_model.h"
#include <Preferences.h>
#include <math.h>

__attribute__((weak)) void imu_hal_set_rotation_locked(bool locked) {
    (void)locked;
}

bool imu_hal_read_tilt(float* pitch_deg, float* roll_deg) {
    float ax, ay, az;
    if (!imu_hal_read_accel(&ax, &ay, &az)) return false;
    *roll_deg  = atan2f(ax, az) * 57.2958f;
    *pitch_deg = atan2f(ay, az) * 57.2958f;
    return true;
}

__attribute__((weak)) bool imu_hal_read_accel_live(float* ax, float* ay, float* az) {
    return imu_hal_read_accel(ax, ay, az);
}

static float g_bias_x = 0.0f;
static float g_bias_y = 0.0f;
static bool  g_has_bias = false;
static bool  g_loaded = false;
static int   g_bias_slot = -1;
static LevelCalibration g_cal = {};

static constexpr const char* LEVEL_PREFS_NS = "level-cal";
static constexpr const char* LEVEL_PREFS_X[2] = {"x0_mg", "x1_mg"};
static constexpr const char* LEVEL_PREFS_Y[2] = {"y0_mg", "y1_mg"};
static constexpr const char* LEVEL_PREFS_ACTIVE = "active";

static void load_gravity_bias(void) {
    if (g_loaded) return;
    g_loaded = true;

    Preferences prefs;
    if (!prefs.begin(LEVEL_PREFS_NS, true)) return;
    const int32_t active = prefs.getInt(LEVEL_PREFS_ACTIVE, -1);
    if (active == 0 || active == 1) {
        g_bias_slot = (int)active;
        g_bias_x = (float)prefs.getInt(LEVEL_PREFS_X[g_bias_slot], 0) / 1000.0f;
        g_bias_y = (float)prefs.getInt(LEVEL_PREFS_Y[g_bias_slot], 0) / 1000.0f;
        g_has_bias = true;
    }
    prefs.end();
}

static bool persist_gravity_bias(float bias_x, float bias_y) {
    Preferences prefs;
    if (!prefs.begin(LEVEL_PREFS_NS, false)) return false;

    // Write the inactive slot first and flip the selector last. A partial NVS
    // failure therefore leaves the previously selected bias intact.
    const int next_slot = g_bias_slot == 0 ? 1 : 0;
    const bool x_ok = prefs.putInt(LEVEL_PREFS_X[next_slot],
                                   (int32_t)lroundf(bias_x * 1000.0f)) == sizeof(int32_t);
    const bool y_ok = x_ok && prefs.putInt(LEVEL_PREFS_Y[next_slot],
                                          (int32_t)lroundf(bias_y * 1000.0f)) == sizeof(int32_t);
    const bool active_ok = y_ok && prefs.putInt(LEVEL_PREFS_ACTIVE, next_slot) == sizeof(int32_t);
    prefs.end();
    if (!active_ok) return false;
    g_bias_slot = next_slot;
    return true;
}

#ifdef IMU_HAL_TESTING
// Clear only volatile HAL state so the host test can model a reboot while
// leaving the fake Preferences store intact. This symbol is absent on device.
void imu_hal_gravity_test_reset(void) {
    g_bias_x = 0.0f;
    g_bias_y = 0.0f;
    g_has_bias = false;
    g_loaded = false;
    g_bias_slot = -1;
    imu_hal_gravity_cal_cancel();
}
#endif

void imu_hal_gravity_cal_begin(void) {
    load_gravity_bias();
    level_calibration_begin(&g_cal);
}

void imu_hal_gravity_cal_reset(void) {
    imu_hal_gravity_cal_cancel();
}

bool imu_hal_gravity_cal_step(int* pct_out) {
    if (!g_cal.active) {
        if (pct_out) *pct_out = 0;
        return false;
    }

    float ax, ay, az;
    if (!imu_hal_read_accel_live(&ax, &ay, &az)) {
        if (pct_out) *pct_out = -1;
        return false;
    }

    float new_bias_x = 0.0f;
    float new_bias_y = 0.0f;
    if (!level_calibration_add(&g_cal, ax, ay, &new_bias_x, &new_bias_y,
                               pct_out)) {
        return false;
    }

    if (!persist_gravity_bias(new_bias_x, new_bias_y)) {
        if (pct_out) *pct_out = -1;
        return false;
    }
    g_bias_x = new_bias_x;
    g_bias_y = new_bias_y;
    g_has_bias = true;
    return true;
}

void imu_hal_gravity_cal_cancel(void) {
    g_cal.active = false;
    g_cal.count = 0;
    g_cal.sum_x = 0.0f;
    g_cal.sum_y = 0.0f;
}

bool imu_hal_gravity_ready(void) {
    load_gravity_bias();
    return g_has_bias;
}

bool imu_hal_read_gravity(float* gx, float* gy) {
    if (!gx || !gy) return false;
    load_gravity_bias();
    float ax, ay, az;
    // Motion apps consume the board driver's rate-limited cache. Reading the
    // I2C sensor on every LVGL frame stalls animation and touch handling.
    if (!imu_hal_read_accel(&ax, &ay, &az)) return false;
    if (g_has_bias) {
        ax -= g_bias_x;
        ay -= g_bias_y;
    }
    // Suppress normal sensor offset/noise even when an app intentionally uses
    // uncalibrated gravity (calibration is only valid on a known-level surface).
    if (fabsf(ax) < 0.05f) ax = 0.0f;
    if (fabsf(ay) < 0.05f) ay = 0.0f;
    *gx = ax;
    *gy = ay;
    return true;
}
