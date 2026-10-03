#include "../../shell/app.h"
#include "../../theme.h"
#include "../../phosphor_cp.h"
#include "../../hal/board_caps.h"
#include "../../hal/imu_hal.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Level — a glass bubble dial. The data path is unchanged from the old
// Gravity Ball (imu_hal_read_gravity: calibration bias + deadzone, the
// QMI8658 X/Y mount exchange, transform back through the frozen display
// rotation). Only the presentation changed: the bouncing ball became a
// spirit-level bubble riding a track ring inside a dial of launcher-grade
// glass (shell/launcher.cpp glass_tile LIFT recipe), with a numeric degree
// readout below and a "Level!" accent flash when the tilt closes under
// 0.5 degrees.

LV_FONT_DECLARE(font_inter_20);   // degree readout — carries U+00B0 (regen range 0x20-0xB0)
LV_FONT_DECLARE(font_inter_14);   // axis labels

#define LEVEL_TICK_MS   20
#define LEVEL_HUE       0x3953c8   // matches AppDef.tile_rgb
#define LEVEL_MAX_DEG   12.0f      // full-scale bubble travel on the track
#define LEVEL_LOCK_DEG  0.5f       // "Level!" flash threshold
#define DEGREE_UTF8     "\xC2\xB0"

#define PHYSICS_INTERVAL_MS 20     // tick cadence kept from the Gravity Ball

static lv_obj_t* dial        = nullptr;
static lv_obj_t* ball        = nullptr;
static lv_obj_t* track_ring  = nullptr;
static lv_obj_t* readout     = nullptr;

static int       dial_size   = 0;
static int       center_c    = 0;   // content-center coord inside the dial
static int       track_r     = 0;
static int       max_off     = 0;
static int       ball_r      = 17;
static float     bubble_x    = 0.0f; // smoothed pixel offsets from center
static float     bubble_y    = 0.0f;
static uint32_t  last_step_ms = 0;
static uint8_t   screen_rotation = 0;
static char      readout_buf[20] = "0.0\xC2\xB0";
static bool      was_level   = false;

// Border-only ring carved in the glass ("engraved"). remove_style_all kills
// the default theme's cream border/background before the designed stroke.
static lv_obj_t* ring(lv_obj_t* parent, int diameter, uint32_t color,
                      lv_opa_t opacity, int width) {
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, diameter, diameter);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(r, opacity, 0);
    lv_obj_set_style_border_width(r, width, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(r);
    return r;
}

// 1px hairline strip — theme-proof crosshair line (no border, bg-only).
static lv_obj_t* hairline(lv_obj_t* parent, int len, bool vertical) {
    lv_obj_t* s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, vertical ? 1 : len, vertical ? len : 1);
    lv_obj_set_style_bg_color(s, RIM_CREAM, 0);
    lv_obj_set_style_bg_opa(s, 16, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_set_style_radius(s, 0, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s);
    return s;
}

static lv_obj_t* axis_label(lv_obj_t* parent, const char* txt) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &font_inter_14, 0);
    lv_obj_set_style_text_color(l, THEME_DIM, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

// The bubble: accent-pool orb with a small bright inner glow dot.
static void build_ball(lv_obj_t* parent, bool compact) {
    ball = lv_obj_create(parent);
    lv_obj_remove_style_all(ball);
    lv_obj_set_size(ball, ball_r * 2, ball_r * 2);
    lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, 0);
    // One opaque VER fill carries the material (composite-opaquely rule):
    // top = terra-cotta washed toward cream light, bottom = deep accent pool.
    const lv_color_t base = lv_color_hex(0xd97757);
    lv_obj_set_style_bg_color(ball, lv_color_mix(base, lv_color_hex(0xffd9bd), 42), 0);
    lv_obj_set_style_bg_grad_color(ball,
        lv_color_mix(base, lv_color_hex(0x2a1208), 26), 0);
    lv_obj_set_style_bg_grad_dir(ball, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(ball, 0, 0);
    lv_obj_set_style_bg_grad_stop(ball, 235, 0);
    lv_obj_set_style_bg_opa(ball, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ball, lv_color_hex(0xffc2ad), 0);
    lv_obj_set_style_border_width(ball, 2, 0);
    lv_obj_set_style_border_opa(ball, 110, 0);
    lv_obj_set_style_pad_all(ball, 0, 0);
    lv_obj_clear_flag(ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ball, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* glow = lv_obj_create(ball);
    lv_obj_remove_style_all(glow);
    lv_obj_set_size(glow, 8, 8);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(glow, lv_color_hex(0xffe9dc), 0);
    lv_obj_set_style_bg_opa(glow, 200, 0);
    lv_obj_set_style_border_width(glow, 0, 0);
    lv_obj_set_style_pad_all(glow, 0, 0);
    lv_obj_clear_flag(glow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(glow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(glow);

    (void)compact;
}

static void place_bubble(float ox, float oy) {
    if (!ball) return;
    lv_obj_set_pos(ball,
        center_c + (int)lroundf(ox) - ball_r,
        center_c + (int)lroundf(oy) - ball_r);
}

static void reset_bubble(void) {
    bubble_x = bubble_y = 0.0f;
    place_bubble(0.0f, 0.0f);
}

static void level_tick(void) {
    const uint32_t now = millis();
    const uint32_t elapsed_ms = now - last_step_ms;
    if (elapsed_ms < PHYSICS_INTERVAL_MS) return;
    last_step_ms = now;

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

    const float mag = sqrtf(screen_x * screen_x + screen_y * screen_y);
    const float mag_c = mag > 0.9999f ? 0.9999f : mag;
    const float tilt_deg = asinf(mag_c) * 57.2958f;

    // Bubble offset along the screen-space tilt direction, linear in tilt,
    // saturating at the track rim (a real bubble's travel, not a pendulum).
    float ox = 0.0f;
    float oy = 0.0f;
    if (mag > 0.001f && track_r > 0) {
        float travel = tilt_deg;
        if (travel > LEVEL_MAX_DEG) travel = LEVEL_MAX_DEG;
        const float scale = (float)max_off / LEVEL_MAX_DEG * travel;
        ox = screen_x / mag * scale;
        oy = screen_y / mag * scale;
    }

    // Light exponential smoothing keeps the bubble glassy without physics.
    constexpr float FOLLOW = 0.28f;
    bubble_x += (ox - bubble_x) * FOLLOW;
    bubble_y += (oy - bubble_y) * FOLLOW;
    place_bubble(bubble_x, bubble_y);

    // Readout + level flash: text/color state flip on the existing widgets.
    const bool level = tilt_deg < LEVEL_LOCK_DEG;
    char buf[20];
    if (level) snprintf(buf, sizeof buf, "Level!");
    else       snprintf(buf, sizeof buf, "%.1f%s", tilt_deg, DEGREE_UTF8);
    if (strcmp(buf, readout_buf) != 0) {
        snprintf(readout_buf, sizeof readout_buf, "%s", buf);
        lv_label_set_text(readout, readout_buf);
    }
    if (level != was_level) {
        lv_obj_set_style_text_color(readout, level ? THEME_ACCENT : THEME_TEXT, 0);
        if (track_ring) {
            lv_obj_set_style_border_color(track_ring,
                level ? THEME_ACCENT : RIM_CREAM, 0);
            lv_obj_set_style_border_opa(track_ring, level ? 100 : 60, 0);
        }
        was_level = level;
    }
}

static bool level_on_button(app_btn_t btn) {
    if (btn != APP_BTN_PRIMARY) return false;
    imu_hal_gravity_cal_reset();
    reset_bubble();
    return true;
}

static void level_create(lv_obj_t* root) {
    const int W = board_caps().width;
    const int H = board_caps().height;
    const int short_edge = W < H ? W : H;
    const bool compact = short_edge < 400;
    const int pill_keep = 34;   // home-pill keep-out: the pill shows inside apps

    // Freeze whichever orientation was active on entry. This avoids a visual
    // jump and keeps the radial UI stable until the user leaves the app.
    imu_hal_set_rotation_locked(true);
    screen_rotation = imu_hal_rotation_quadrant();

    // Do not learn a bias from the entry pose: the horizontal components are
    // real gravity unless the device is known to be lying level. Resetting the
    // optional calibration makes this app use raw gravity plus a deadzone.
    imu_hal_gravity_cal_reset();

    // Root duotone, same anti-banding shape as settings/soon (VER falloff,
    // held floor for AMOLED battery).
    lv_obj_set_style_bg_color(root, THEME_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(root, THEME_BG_BOT, 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(root, 0, 0);
    lv_obj_set_style_bg_grad_stop(root, 230, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    dial_size = compact ? short_edge - 104 : 340;
    ball_r    = compact ? 14 : 17;

    const int track_d  = dial_size * 7 / 10;
    const int ring1_d  = dial_size * 22 / 25;
    const int ring2_d  = dial_size * 47 / 100;
    const int hair_len = track_d + 10;
    track_r  = track_d / 2;
    center_c = dial_size / 2 - 2;              // content center (2px border)
    max_off  = track_r - ball_r - 6;
    if (max_off < 8) max_off = 8;

    const int usable   = H - pill_keep;
    const int dial_top = (usable - dial_size) / 2;

    // ---- The dial: launcher glass_tile recipe on a circular body ----------
    dial = lv_obj_create(root);
    lv_obj_set_size(dial, dial_size, dial_size);
    lv_obj_align(dial, LV_ALIGN_TOP_MID, 0, dial_top);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);

    // LIFT fill: the wallpaper's own light added-to. Top stop = wall lifted
    // hard toward cream (sheen), pool stop = wall lifted toward the hue.
    // Critic r1: dial read flat/murky cold blue. Chroma-dominant fix:
    // cream-lit top (the tile blue demotes to a cast), pool keeps the
    // hue story; rim + body carry the material.
    const lv_color_t hue  = lv_color_hex(LEVEL_HUE);
    const lv_color_t wall = lv_color_mix(THEME_BG_BOT, THEME_BG_TOP, 140);
    // The launcher's EXACT fill numbers (the house chroma-dominant ramp —
    // proven anti-banding by critics r6-r8; banding appeared only when the
    // stops drifted off-recipe). The dial keeps the blue tile story as the
    // pool; the top carries the cream sheen.
    lv_obj_set_style_bg_color(dial,
        lv_color_mix(lv_color_mix(wall, RIM_CREAM, 85), hue, 208), 0);
    lv_obj_set_style_bg_opa(dial, 145, 0);
    // Chroma-flow over luma-flow (r7 at dial scale): the pool = saturated
    // pale blue vs the cream top — the luma ramp compresses while the eye
    // rides the hue shift (16bpp plateaus hide in chroma).
    lv_obj_set_style_bg_grad_color(dial,
        lv_color_mix(lv_color_mix(wall, lv_color_hex(0xffffff), 12), hue, 165), 0);
    lv_obj_set_style_bg_grad_opa(dial, 185, 0);
    lv_obj_set_style_bg_grad_dir(dial, LV_GRAD_DIR_VER, 0);

    lv_obj_set_style_bg_main_stop(dial, 0, 0);
    lv_obj_set_style_bg_grad_stop(dial, 240, 0);

    // Soft puddle shadow (width 14 / ofs_y 5 / spread 0 / opa 25).
    lv_obj_set_style_shadow_color(dial, GLASS_SHADOW, 0);
    lv_obj_set_style_shadow_width(dial, 14, 0);
    lv_obj_set_style_shadow_spread(dial, 0, 0);
    lv_obj_set_style_shadow_ofs_y(dial, 5, 0);
    lv_obj_set_style_shadow_opa(dial, 25, 0);

    // Cream rim ON the edge + dark outer contour to seat it on the wallpaper.
    lv_obj_set_style_border_color(dial, RIM_CREAM, 0);
    lv_obj_set_style_border_width(dial, 3, 0);
    lv_obj_set_style_border_opa(dial, 150, 0);
    lv_obj_set_style_outline_color(dial, GLASS_SHADOW, 0);
    lv_obj_set_style_outline_width(dial, 1, 0);
    lv_obj_set_style_outline_pad(dial, 0, 0);
    lv_obj_set_style_outline_opa(dial, 90, 0);
    lv_obj_set_style_pad_all(dial, 0, 0);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_CLICKABLE);
    // Directional rim (critic r2b): light from above — the round-form glint
    // (voice's proven arc): a concentric circle child drawing only its top
    // border, so the sheen follows the curvature instead of a uniform edge.
    lv_obj_t* glint = lv_obj_create(dial);
    lv_obj_set_size(glint, dial_size - 12, dial_size - 12);
    lv_obj_align(glint, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_radius(glint, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(glint, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(glint, RIM_CREAM, 0);
    lv_obj_set_style_border_width(glint, 3, 0);
    lv_obj_set_style_border_side(glint, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_opa(glint, 110, 0);
    lv_obj_set_style_outline_width(glint, 0, 0);
    lv_obj_set_style_pad_all(glint, 0, 0);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(glint, LV_OBJ_FLAG_IGNORE_LAYOUT);

    // ---- Engraved instrument work inside the dial -------------------------
    ring(dial, ring1_d, 0xf2e6da, 18, 1);
    ring(dial, ring2_d, 0xf2e6da, 25, 1);
    hairline(dial, hair_len, false);
    hairline(dial, hair_len, true);

    // Bubble-track ring: the affordance the bubble rides; recolored by the
    // level flash (color state flip only).
    track_ring = ring(dial, track_d, 0xf2e6da, 60, 2);

    // Axis letters H/V-cardinal, Inter 14, dim — sitting between track ring
    // and the outer engraved ring.
    const int label_r = (ring1_d / 2) - 18;
    const int ax_off  = center_c - label_r - 7;
    lv_obj_t* l_top = axis_label(dial, "F");
    lv_obj_align(l_top, LV_ALIGN_TOP_MID, 0, ax_off);
    lv_obj_t* l_bot = axis_label(dial, "B");
    lv_obj_align(l_bot, LV_ALIGN_BOTTOM_MID, 0, -(ax_off + 7) + 7);
    lv_obj_t* l_left = axis_label(dial, "L");
    lv_obj_align(l_left, LV_ALIGN_LEFT_MID, ax_off, 0);
    lv_obj_t* l_right = axis_label(dial, "R");
    lv_obj_align(l_right, LV_ALIGN_RIGHT_MID, -(ax_off + 7) + 7, 0);

    // Center pin: the exact-level mark under the bubble.
    lv_obj_t* pin = lv_obj_create(dial);
    lv_obj_remove_style_all(pin);
    lv_obj_set_size(pin, 6, 6);
    lv_obj_set_style_radius(pin, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pin, lv_color_hex(0xf2e6da), 0);
    lv_obj_set_style_bg_opa(pin, 70, 0);
    lv_obj_set_style_border_width(pin, 0, 0);
    lv_obj_clear_flag(pin, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(pin);

    // The bubble on top of the instrument work.
    build_ball(dial, compact);

    // ---- Numeric degree readout under the dial ----------------------------
    readout = lv_label_create(root);
    lv_obj_set_width(readout, W);           // pin width: no per-tick reflow drift
    lv_label_set_text(readout, readout_buf);
    lv_obj_set_style_text_font(readout, &font_inter_20, 0);
    lv_obj_set_style_text_color(readout, THEME_TEXT, 0);
    lv_obj_set_style_text_align(readout, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(readout, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_border_width(readout, 0, 0);
    const int label_y = dial_top + dial_size +
        (usable - dial_top - dial_size - 24) / 2;
    lv_obj_align(readout, LV_ALIGN_TOP_MID, 0, label_y);

    reset_bubble();
    last_step_ms = millis();
}

static void level_destroy(void) {
    imu_hal_set_rotation_locked(false);
    dial = nullptr;
    ball = nullptr;
    track_ring = nullptr;
    readout = nullptr;
    bubble_x = bubble_y = 0.0f;
    was_level = false;
    snprintf(readout_buf, sizeof readout_buf, "0.0%s", DEGREE_UTF8);
}

extern const AppDef app_level = {
    .id            = "level",
    .title         = "Level",
    .glyph         = PH_MAP_PIN,
    .tile_rgb      = 0x3953c8,
    .blurb         = "A glass bubble dial with a live degree readout.",
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