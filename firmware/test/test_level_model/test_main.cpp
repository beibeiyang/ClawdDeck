#include "apps/level/level_model.h"

#include <cmath>
#include <cstdio>

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

static void flat_projects_to_center() {
    const LevelProjection p = level_project(0.0f, 0.0f, 0, 120.0f);
    CHECK(near(p.x, 0.0f));
    CHECK(near(p.y, 0.0f));
    CHECK(near(p.tilt_deg, 0.0f));
}

static void upright_projects_to_six_oclock() {
    const LevelProjection p = level_project(0.0f, 1.0f, 0, 120.0f);
    CHECK(near(p.x, 0.0f));
    CHECK(near(p.y, 120.0f));
    CHECK(near(p.tilt_deg, 90.0f, 0.01f));
}

static void physical_low_edge_survives_all_rotations() {
    const LevelProjection r0 = level_project(0.0f, 0.5f, 0, 100.0f);
    const LevelProjection r1 = level_project(0.0f, 0.5f, 1, 100.0f);
    const LevelProjection r2 = level_project(0.0f, 0.5f, 2, 100.0f);
    const LevelProjection r3 = level_project(0.0f, 0.5f, 3, 100.0f);

    CHECK(near(r0.x, 0.0f) && r0.y > 0.0f);
    CHECK(r1.x > 0.0f && near(r1.y, 0.0f));
    CHECK(near(r2.x, 0.0f) && r2.y < 0.0f);
    CHECK(r3.x < 0.0f && near(r3.y, 0.0f));
}

static void travel_saturates_at_track_radius() {
    const LevelProjection p = level_project(3.0f, 4.0f, 0, 72.0f);
    CHECK(near(std::sqrt(p.x * p.x + p.y * p.y), 72.0f, 0.01f));
    CHECK(near(p.tilt_deg, 90.0f, 0.01f));
}

static void calibration_averages_mount_bias() {
    LevelCalibration cal;
    level_calibration_begin(&cal);

    float bias_x = 99.0f;
    float bias_y = 99.0f;
    int pct = -1;
    bool complete = false;
    for (int i = 0; i < LEVEL_CAL_SAMPLES; ++i) {
        const float x = (i & 1) ? 0.10f : 0.06f;
        const float y = (i & 1) ? -0.02f : -0.04f;
        complete = level_calibration_add(&cal, x, y, &bias_x, &bias_y, &pct);
    }

    CHECK(complete);
    CHECK(near(bias_x, 0.08f));
    CHECK(near(bias_y, -0.03f));
    CHECK(pct == 100);
}

static void incomplete_calibration_emits_no_bias() {
    LevelCalibration cal;
    level_calibration_begin(&cal);

    float bias_x = 12.5f;
    float bias_y = -4.5f;
    int pct = -1;
    for (int i = 0; i < LEVEL_CAL_SAMPLES - 1; ++i) {
        CHECK(!level_calibration_add(&cal, 0.1f, -0.2f,
                                     &bias_x, &bias_y, &pct));
    }

    CHECK(near(bias_x, 12.5f));
    CHECK(near(bias_y, -4.5f));
    CHECK(pct < 100);
}

int main() {
    flat_projects_to_center();
    upright_projects_to_six_oclock();
    physical_low_edge_survives_all_rotations();
    travel_saturates_at_track_radius();
    calibration_averages_mount_bias();
    incomplete_calibration_emits_no_bias();

    if (failures) {
        std::printf("%d level model check(s) failed\n", failures);
        return 1;
    }
    std::printf("all level model checks passed\n");
    return 0;
}
