#include "../../shell/app.h"
#include "../../theme.h"
#include "../../phosphor_cp.h"
#include "../../hal/board_caps.h"
#include "../../hal/imu_hal.h"
#include "../../hal/input_hal.h"
#include "level_model.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_inter_14);
LV_FONT_DECLARE(font_inter_16);
LV_FONT_DECLARE(font_inter_24);
LV_FONT_DECLARE(font_inter_32);

namespace {

constexpr uint32_t PHYSICS_INTERVAL_MS = 20;
constexpr uint32_t CAL_HOLD_MS = 1500;
constexpr uint32_t CAL_SAMPLE_INTERVAL_MS = 50;
constexpr float LEVEL_LOCK_DEG = 0.5f;
constexpr float VERTICAL_DEG = 75.0f;
constexpr float FOLLOW = 0.22f;
constexpr const char* DEGREE_UTF8 = "\xC2\xB0";

constexpr uint32_t COLOR_ROOT = 0x050607;
constexpr uint32_t COLOR_DIAL = 0x101215;
constexpr uint32_t COLOR_DIAL_EDGE = 0x2d3136;
constexpr uint32_t COLOR_TRACK = 0x50545a;
constexpr uint32_t COLOR_CROSSHAIR = 0x34383d;
constexpr uint32_t COLOR_BALL = 0xd97757;

lv_obj_t* dial = nullptr;
lv_obj_t* ball = nullptr;
lv_obj_t* track_ring = nullptr;
lv_obj_t* center_pin = nullptr;
lv_obj_t* readout = nullptr;
lv_obj_t* state_label = nullptr;

int dial_size = 0;
int max_offset = 0;
int ball_radius = 16;
float ball_x = 0.0f;
float ball_y = 0.0f;
uint8_t screen_rotation = 0;
uint32_t last_step_ms = 0;

bool cal_armed = false;
bool calibrating = false;
uint32_t cal_hold_started_ms = 0;
uint32_t cal_last_sample_ms = 0;
char readout_text[20] = "";
char state_text[48] = "";

void make_passive(lv_obj_t* obj) {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* make_ring(lv_obj_t* parent, int diameter, uint32_t color,
                    lv_opa_t opacity, int width) {
    lv_obj_t* ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, diameter, diameter);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(ring, opacity, 0);
    lv_obj_set_style_border_width(ring, width, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    make_passive(ring);
    lv_obj_center(ring);
    return ring;
}

lv_obj_t* make_hairline(lv_obj_t* parent, int length, bool vertical) {
    lv_obj_t* line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, vertical ? 1 : length, vertical ? length : 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(COLOR_CROSSHAIR), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    make_passive(line);
    lv_obj_center(line);
    return line;
}

void set_state(const char* text, lv_color_t color) {
    if (!state_label) return;
    if (strcmp(state_text, text) != 0) {
        snprintf(state_text, sizeof state_text, "%s", text);
        lv_label_set_text(state_label, state_text);
    }
    lv_obj_set_style_text_color(state_label, color, 0);
}

void set_readout(float degrees) {
    if (!readout) return;
    char next[20];
    snprintf(next, sizeof next, "%.1f%s", degrees, DEGREE_UTF8);
    if (strcmp(readout_text, next) != 0) {
        snprintf(readout_text, sizeof readout_text, "%s", next);
        lv_label_set_text(readout, readout_text);
    }
}

void place_ball(float x, float y) {
    if (!ball) return;
    lv_obj_align(ball, LV_ALIGN_CENTER, (int)lroundf(x), (int)lroundf(y));
}

void update_live_state(float tilt_deg) {
    const bool level = tilt_deg < LEVEL_LOCK_DEG;
    const lv_color_t lock_color = level ? THEME_GREEN : lv_color_hex(0x62676d);
    lv_obj_set_style_bg_color(center_pin, lock_color, 0);

    if (!imu_hal_gravity_ready()) {
        set_state("HOLD BOOT 1.5s  KEEP FLAT", THEME_DIM);
    } else if (level) {
        set_state("LEVEL", THEME_GREEN);
    } else if (tilt_deg >= VERTICAL_DEG) {
        set_state("VERTICAL", THEME_TEXT);
    } else {
        set_state("TILTED", THEME_DIM);
    }
}

bool calibration_tick(uint32_t now) {
    if (cal_armed && !calibrating) {
        if (!input_hal_is_held(INPUT_BTN_PRIMARY)) {
            cal_armed = false;
            return false;
        }
        if (now - cal_hold_started_ms < CAL_HOLD_MS) {
            set_state("KEEP FLAT · HOLD", THEME_DIM);
            return false;
        }

        cal_armed = false;
        calibrating = true;
        cal_last_sample_ms = 0;
        imu_hal_gravity_cal_begin();
        set_state("CALIBRATING 0%", THEME_ACCENT);
    }

    if (!calibrating) return false;
    if (cal_last_sample_ms && now - cal_last_sample_ms < CAL_SAMPLE_INTERVAL_MS) {
        return true;
    }
    cal_last_sample_ms = now;

    int pct = 0;
    const bool complete = imu_hal_gravity_cal_step(&pct);
    if (pct < 0) {
        calibrating = false;
        imu_hal_gravity_cal_cancel();
        set_state("CALIBRATION FAILED", THEME_RED);
        return true;
    }

    char progress[32];
    snprintf(progress, sizeof progress, "CALIBRATING %d%%", pct);
    set_state(progress, THEME_ACCENT);
    if (complete) {
        calibrating = false;
        ball_x = 0.0f;
        ball_y = 0.0f;
        place_ball(0.0f, 0.0f);
        set_state("CALIBRATED", THEME_GREEN);
    }
    return true;
}

void level_tick() {
    const uint32_t now = millis();
    if (calibration_tick(now)) return;
    if (now - last_step_ms < PHYSICS_INTERVAL_MS) return;
    last_step_ms = now;

    float gravity_x = 0.0f;
    float gravity_y = 0.0f;
    if (!imu_hal_read_gravity(&gravity_x, &gravity_y)) {
        set_state("SENSOR UNAVAILABLE", THEME_RED);
        return;
    }

    const LevelProjection projection = level_project_sensor(
        gravity_x, gravity_y, screen_rotation, (float)max_offset);

    ball_x += (projection.x - ball_x) * FOLLOW;
    ball_y += (projection.y - ball_y) * FOLLOW;
    place_ball(ball_x, ball_y);
    set_readout(projection.tilt_deg);
    update_live_state(projection.tilt_deg);
}

bool level_on_button(app_btn_t btn) {
    if (btn != APP_BTN_PRIMARY) return false;
    if (calibrating) return true;
    cal_armed = true;
    cal_hold_started_ms = millis();
    set_state("KEEP FLAT · HOLD", THEME_DIM);
    return true;
}

void level_create(lv_obj_t* root) {
    const int width = board_caps().width;
    const int height = board_caps().height;
    const int short_edge = width < height ? width : height;
    const bool compact = short_edge < 400;

    imu_hal_set_rotation_locked(true);
    screen_rotation = imu_hal_rotation_quadrant();

    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_ROOT), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    readout = lv_label_create(root);
    lv_obj_set_size(readout, width - 40, compact ? 36 : 42);
    lv_obj_align(readout, LV_ALIGN_TOP_MID, 0, compact ? 32 : 38);
    lv_obj_set_style_text_font(readout, compact ? &font_inter_24 : &font_inter_32, 0);
    lv_obj_set_style_text_color(readout, THEME_TEXT, 0);
    lv_obj_set_style_text_align(readout, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(readout, LV_LABEL_LONG_CLIP);
    make_passive(readout);

    dial_size = compact ? short_edge - 86 : 314;
    ball_radius = compact ? 13 : 16;
    const int dial_top = compact ? 84 : 90;
    const int track_diameter = dial_size - (compact ? 44 : 52);
    max_offset = track_diameter / 2 - ball_radius - 3;

    dial = lv_obj_create(root);
    lv_obj_set_size(dial, dial_size, dial_size);
    lv_obj_align(dial, LV_ALIGN_TOP_MID, 0, dial_top);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dial, lv_color_hex(COLOR_DIAL), 0);
    lv_obj_set_style_bg_grad_dir(dial, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(dial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dial, lv_color_hex(COLOR_DIAL_EDGE), 0);
    lv_obj_set_style_border_width(dial, 1, 0);
    lv_obj_set_style_border_opa(dial, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(dial, 0, 0);
    make_passive(dial);

    make_hairline(dial, track_diameter, false);
    make_hairline(dial, track_diameter, true);
    track_ring = make_ring(dial, track_diameter, COLOR_TRACK, 150, 1);

    center_pin = lv_obj_create(dial);
    lv_obj_remove_style_all(center_pin);
    lv_obj_set_size(center_pin, compact ? 7 : 8, compact ? 7 : 8);
    lv_obj_set_style_radius(center_pin, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center_pin, lv_color_hex(0x62676d), 0);
    lv_obj_set_style_bg_opa(center_pin, LV_OPA_COVER, 0);
    make_passive(center_pin);
    lv_obj_center(center_pin);

    ball = lv_obj_create(dial);
    lv_obj_remove_style_all(ball);
    lv_obj_set_size(ball, ball_radius * 2, ball_radius * 2);
    lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ball, lv_color_hex(COLOR_BALL), 0);
    lv_obj_set_style_bg_grad_dir(ball, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(ball, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ball, lv_color_hex(0xf2a184), 0);
    lv_obj_set_style_border_width(ball, 1, 0);
    lv_obj_set_style_border_opa(ball, 170, 0);
    lv_obj_set_style_shadow_color(ball, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(ball, 8, 0);
    lv_obj_set_style_shadow_opa(ball, 90, 0);
    make_passive(ball);

    state_label = lv_label_create(root);
    lv_obj_set_size(state_label, width - 40, compact ? 34 : 30);
    lv_obj_align(state_label, LV_ALIGN_TOP_MID, 0,
                 dial_top + dial_size + (compact ? 8 : 10));
    lv_obj_set_style_text_font(state_label,
                               compact ? &font_inter_14 : &font_inter_16, 0);
    lv_obj_set_style_text_letter_space(state_label, 1, 0);
    lv_obj_set_style_text_align(state_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(state_label, LV_LABEL_LONG_CLIP);
    make_passive(state_label);

    ball_x = 0.0f;
    ball_y = 0.0f;
    place_ball(0.0f, 0.0f);
    set_readout(0.0f);
    set_state(imu_hal_gravity_ready() ? "LEVEL" : "HOLD BOOT 1.5s  KEEP FLAT",
              imu_hal_gravity_ready() ? THEME_GREEN : THEME_DIM);
    last_step_ms = millis();
}

void level_destroy() {
    if (cal_armed || calibrating) imu_hal_gravity_cal_cancel();
    imu_hal_set_rotation_locked(false);

    dial = nullptr;
    ball = nullptr;
    track_ring = nullptr;
    center_pin = nullptr;
    readout = nullptr;
    state_label = nullptr;
    ball_x = 0.0f;
    ball_y = 0.0f;
    cal_armed = false;
    calibrating = false;
    readout_text[0] = '\0';
    state_text[0] = '\0';
}

}  // namespace

extern const AppDef app_level = {
    .id            = "level",
    .title         = "Level",
    .glyph         = PH_MAP_PIN,
    .tile_rgb      = 0x3953c8,
    .blurb         = "A calibrated instrument that follows gravity.",
    .required_caps = APP_CAP_IMU,
    .persistent    = false,
    .immersive     = true,
    .create        = level_create,
    .destroy       = level_destroy,
    .tick          = level_tick,
    .on_button     = level_on_button,
    .on_usage      = nullptr,
    .on_ble        = nullptr,
    .on_battery    = nullptr,
};
