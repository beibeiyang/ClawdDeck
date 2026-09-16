#include "../../shell/app.h"
#include "../../theme.h"
#include "../../hal/board_caps.h"
#include "../../hal/imu_hal.h"
#include <Arduino.h>
#include <math.h>

// Orientation-neutral Gravity Ball. The display orientation is frozen only
// while this app is open; the device-frame gravity vector is transformed into
// that frozen frame so the ball still falls toward the physical low edge.

#define PHYSICS_INTERVAL_MS 20
#define GRAVITY_ACCEL       760.0f  // g -> px/s^2
#define WALL_BOUNCE         0.42f

static lv_obj_t* arena        = nullptr;
static lv_obj_t* ball         = nullptr;
static int       arena_size   = 0;
static int       ball_r       = 24;
static float     ball_x       = 0.0f;
static float     ball_y       = 0.0f;
static float     velocity_x   = 0.0f;
static float     velocity_y   = 0.0f;
static uint32_t  last_step_ms = 0;
static uint8_t   screen_rotation = 0;

static lv_obj_t* make_ring(lv_obj_t* parent, int diameter, uint32_t color,
                           lv_opa_t opacity, int width) {
    lv_obj_t* ring = lv_obj_create(parent);
    lv_obj_set_size(ring, diameter, diameter);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(ring, opacity, 0);
    lv_obj_set_style_border_width(ring, width, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(ring);
    return ring;
}

static void make_axis(lv_obj_t* parent, int w, int h) {
    lv_obj_t* axis = lv_obj_create(parent);
    lv_obj_set_size(axis, w, h);
    lv_obj_set_style_bg_color(axis, lv_color_hex(0x34322f), 0);
    lv_obj_set_style_bg_opa(axis, LV_OPA_50, 0);
    lv_obj_set_style_border_width(axis, 0, 0);
    lv_obj_set_style_radius(axis, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(axis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(axis, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(axis);
}

static void reset_ball(void) {
    ball_x = arena_size * 0.5f;
    ball_y = arena_size * 0.5f;
    velocity_x = 0.0f;
    velocity_y = 0.0f;
    if (ball) {
        lv_obj_set_pos(ball,
            (int)lroundf(ball_x) - ball_r,
            (int)lroundf(ball_y) - ball_r);
    }
}

static void constrain_to_arena(void) {
    const float center = arena_size * 0.5f;
    const float limit  = center - ball_r - 7.0f;
    const float dx = ball_x - center;
    const float dy = ball_y - center;
    const float distance_sq = dx * dx + dy * dy;
    if (distance_sq <= limit * limit) return;

    const float distance = sqrtf(distance_sq);
    if (distance < 0.001f) return;
    const float nx = dx / distance;
    const float ny = dy / distance;
    ball_x = center + nx * limit;
    ball_y = center + ny * limit;

    // Reflect only the outward component. Tangential speed survives, so the
    // ball naturally rolls around the circular rim instead of sticking.
    const float outward = velocity_x * nx + velocity_y * ny;
    if (outward > 0.0f) {
        velocity_x -= (1.0f + WALL_BOUNCE) * outward * nx;
        velocity_y -= (1.0f + WALL_BOUNCE) * outward * ny;
    }
}

static void level_tick(void) {
    const uint32_t now = millis();
    const uint32_t elapsed_ms = now - last_step_ms;
    if (elapsed_ms < PHYSICS_INTERVAL_MS) return;
    last_step_ms = now;
    const float dt = ((elapsed_ms > 50 ? 50 : elapsed_ms) * 0.001f);

    float gx = 0.0f;
    float gy = 0.0f;
    if (!imu_hal_read_gravity(&gx, &gy)) return;

    // QMI8658 is mounted with X/Y exchanged relative to panel coordinates:
    // panel +X = sensor +Y, panel +Y = sensor +X. Convert that physical-panel
    // vector back through the display rotation frozen when this app opened.
    const float panel_x = gy;
    const float panel_y = gx;
    float screen_x = panel_x;
    float screen_y = panel_y;
    switch (screen_rotation) {
    case 1: screen_x =  panel_y; screen_y = -panel_x; break;
    case 2: screen_x = -panel_x; screen_y = -panel_y; break;
    case 3: screen_x = -panel_y; screen_y =  panel_x; break;
    default: break;
    }
    velocity_x += screen_x * GRAVITY_ACCEL * dt;
    velocity_y += screen_y * GRAVITY_ACCEL * dt;

    // Frame-rate-independent rolling resistance.
    const float damping = powf(0.18f, dt);
    velocity_x *= damping;
    velocity_y *= damping;
    ball_x += velocity_x * dt;
    ball_y += velocity_y * dt;
    constrain_to_arena();

    if (ball) {
        lv_obj_set_pos(ball,
            (int)lroundf(ball_x) - ball_r,
            (int)lroundf(ball_y) - ball_r);
    }
}

static bool level_on_button(app_btn_t btn) {
    if (btn != APP_BTN_PRIMARY) return false;
    reset_ball();
    return true;
}

static void level_create(lv_obj_t* root) {
    const int W = board_caps().width;
    const int H = board_caps().height;
    const int short_edge = W < H ? W : H;
    const bool compact = short_edge < 400;

    // Freeze whichever orientation was active on entry. This avoids a visual
    // jump and keeps the radial UI stable until the user leaves the app.
    imu_hal_set_rotation_locked(true);
    screen_rotation = imu_hal_rotation_quadrant();

    arena_size = short_edge - (compact ? 24 : 40);
    ball_r = compact ? 20 : 28;

    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    arena = lv_obj_create(root);
    lv_obj_set_size(arena, arena_size, arena_size);
    lv_obj_set_style_radius(arena, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(arena, lv_color_hex(0x11110f), 0);
    lv_obj_set_style_bg_opa(arena, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(arena, lv_color_hex(0x4a4640), 0);
    lv_obj_set_style_border_width(arena, compact ? 2 : 3, 0);
    lv_obj_set_style_pad_all(arena, 0, 0);
    lv_obj_clear_flag(arena, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(arena);

    // Radial geometry stays visually balanced at every physical orientation.
    make_axis(arena, arena_size - 34, 2);
    make_axis(arena, 2, arena_size - 34);
    make_ring(arena, arena_size * 3 / 4, 0x4a4640, LV_OPA_40, 2);
    make_ring(arena, arena_size / 2,     0x4a4640, LV_OPA_30, 2);
    make_ring(arena, arena_size / 4,     0x6b655d, LV_OPA_30, 2);

    lv_obj_t* center_dot = lv_obj_create(arena);
    const int dot_size = compact ? 8 : 10;
    lv_obj_set_size(center_dot, dot_size, dot_size);
    lv_obj_set_style_radius(center_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center_dot, lv_color_hex(0x7f786e), 0);
    lv_obj_set_style_bg_opa(center_dot, LV_OPA_60, 0);
    lv_obj_set_style_border_width(center_dot, 0, 0);
    lv_obj_clear_flag(center_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(center_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(center_dot);

    ball = lv_obj_create(arena);
    lv_obj_set_size(ball, ball_r * 2, ball_r * 2);
    lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ball, THEME_ACCENT, 0);
    lv_obj_set_style_bg_opa(ball, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ball, lv_color_hex(0xffc2ad), 0);
    lv_obj_set_style_border_width(ball, compact ? 2 : 3, 0);
    lv_obj_set_style_pad_all(ball, 0, 0);
    lv_obj_clear_flag(ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ball, LV_OBJ_FLAG_CLICKABLE);

    // A centered core keeps the orb legible without introducing an "up" side.
    lv_obj_t* core = lv_obj_create(ball);
    const int core_size = compact ? 10 : 14;
    lv_obj_set_size(core, core_size, core_size);
    lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(core, lv_color_hex(0xffe8de), 0);
    lv_obj_set_style_bg_opa(core, LV_OPA_70, 0);
    lv_obj_set_style_border_width(core, 0, 0);
    lv_obj_clear_flag(core, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(core, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(core);

    // Do not learn a bias from the entry pose: the horizontal components are
    // real gravity unless the device is known to be lying level. Resetting the
    // optional calibration makes this app use raw gravity plus a deadzone.
    imu_hal_gravity_cal_reset();
    reset_ball();
    last_step_ms = millis();
}

static void level_destroy(void) {
    imu_hal_set_rotation_locked(false);
    arena = nullptr;
    ball = nullptr;
    velocity_x = velocity_y = 0.0f;
}

extern const AppDef app_level = {
    .id            = "level",
    .title         = "Gravity Ball",
    .glyph         = LV_SYMBOL_GPS,
    .tile_rgb      = 0x4a5fc1,
    .blurb         = "A text-free gravity playground for every orientation.",
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
