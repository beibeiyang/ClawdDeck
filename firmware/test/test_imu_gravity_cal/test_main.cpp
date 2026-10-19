#include "hal/imu_hal.h"
#include "apps/level/level_model.h"
#include <Preferences.h>

#include <cmath>
#include <cstdio>
#include <cstring>

static float sample_x = 0.0f;
static float sample_y = 0.0f;
static int persisted_writes = 0;
static int fail_on_write = 0;
static const char* pref_keys[] = {"x0_mg", "y0_mg", "x1_mg", "y1_mg", "active"};
static int32_t pref_values[5] = {};
static bool pref_present[5] = {};

extern void imu_hal_gravity_test_reset(void);

bool imu_hal_read_accel(float* ax, float* ay, float* az) {
    *ax = sample_x;
    *ay = sample_y;
    *az = 1.0f;
    return true;
}

static int pref_index(const char* key) {
    for (int i = 0; i < 5; ++i) {
        if (std::strcmp(key, pref_keys[i]) == 0) return i;
    }
    return -1;
}

int32_t Preferences::getInt(const char* key, int32_t default_value) {
    const int index = pref_index(key);
    return index >= 0 && pref_present[index] ? pref_values[index] : default_value;
}

size_t Preferences::putInt(const char* key, int32_t value) {
    if (fail_on_write && persisted_writes + 1 == fail_on_write) return 0;
    ++persisted_writes;
    const int index = pref_index(key);
    if (index < 0) return 0;
    pref_values[index] = value;
    pref_present[index] = true;
    return sizeof(int32_t);
}

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static bool near(float actual, float expected, float tolerance = 0.001f) {
    return std::fabs(actual - expected) <= tolerance;
}

static void cancelled_calibration_does_not_persist() {
    imu_hal_gravity_cal_begin();
    int pct = -1;
    for (int i = 0; i < LEVEL_CAL_SAMPLES - 1; ++i) {
        CHECK(!imu_hal_gravity_cal_step(&pct));
    }
    imu_hal_gravity_cal_cancel();
    CHECK(persisted_writes == 0);
}

static void complete_calibration_persists_and_corrects_gravity() {
    sample_x = 0.08f;
    sample_y = -0.03f;
    imu_hal_gravity_cal_begin();

    int pct = -1;
    bool complete = false;
    for (int i = 0; i < LEVEL_CAL_SAMPLES; ++i) {
        complete = imu_hal_gravity_cal_step(&pct);
    }
    CHECK(complete);
    CHECK(pct == 100);
    CHECK(imu_hal_gravity_ready());
    CHECK(persisted_writes == 3);

    sample_x = 0.18f;
    sample_y = -0.13f;
    float gx = 0.0f;
    float gy = 0.0f;
    CHECK(imu_hal_read_gravity(&gx, &gy));
    CHECK(near(gx, 0.10f));
    CHECK(near(gy, -0.10f));
}

static void calibration_store_failure_keeps_last_good_bias() {
    // The inactive slot's X write succeeds, then its Y write fails. The
    // selector must continue pointing at the previous complete slot.
    fail_on_write = persisted_writes + 2;
    sample_x = 0.50f;
    sample_y = 0.50f;
    imu_hal_gravity_cal_begin();

    int pct = -1;
    bool complete = false;
    for (int i = 0; i < LEVEL_CAL_SAMPLES; ++i) {
        complete = imu_hal_gravity_cal_step(&pct);
    }
    CHECK(!complete);
    CHECK(pct == -1);

    sample_x = 0.18f;
    sample_y = -0.13f;
    float gx = 0.0f;
    float gy = 0.0f;
    CHECK(imu_hal_read_gravity(&gx, &gy));
    CHECK(near(gx, 0.10f));
    CHECK(near(gy, -0.10f));
    fail_on_write = 0;
}

static void persisted_bias_reloads_after_hal_reinitialization() {
    imu_hal_gravity_test_reset();
    CHECK(imu_hal_gravity_ready());
    sample_x = 0.18f;
    sample_y = -0.13f;
    float gx = 0.0f;
    float gy = 0.0f;
    CHECK(imu_hal_read_gravity(&gx, &gy));
    CHECK(near(gx, 0.10f));
    CHECK(near(gy, -0.10f));
}

int main() {
    cancelled_calibration_does_not_persist();
    complete_calibration_persists_and_corrects_gravity();
    calibration_store_failure_keeps_last_good_bias();
    persisted_bias_reloads_after_hal_reinitialization();

    if (failures) {
        std::printf("%d IMU calibration check(s) failed\n", failures);
        return 1;
    }
    std::printf("all IMU calibration checks passed\n");
    return 0;
}
