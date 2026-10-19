#include "hal/imu_hal.h"
#include "apps/level/level_model.h"
#include <Preferences.h>

#include <cmath>
#include <cstdio>

static float sample_x = 0.0f;
static float sample_y = 0.0f;
static int persisted_writes = 0;
static bool persist_should_fail = false;

bool imu_hal_read_accel(float* ax, float* ay, float* az) {
    *ax = sample_x;
    *ay = sample_y;
    *az = 1.0f;
    return true;
}

int32_t Preferences::getInt(const char*, int32_t default_value) {
    return default_value;
}

size_t Preferences::putInt(const char*, int32_t) {
    if (persist_should_fail) return 0;
    ++persisted_writes;
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
    persist_should_fail = true;
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
    persist_should_fail = false;
}

int main() {
    cancelled_calibration_does_not_persist();
    complete_calibration_persists_and_corrects_gravity();
    calibration_store_failure_keeps_last_good_bias();

    if (failures) {
        std::printf("%d IMU calibration check(s) failed\n", failures);
        return 1;
    }
    std::printf("all IMU calibration checks passed\n");
    return 0;
}
