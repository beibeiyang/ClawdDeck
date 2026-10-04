#include "statusbar.h"
#include "phosphor_cp.h"
#include "clock_src.h"
#include "../theme.h"
#include "../hal/board_caps.h"
#include <stdio.h>
#include <string.h>

// Apple-grade status type: Inter Medium (SIL OFL; mirrors the old styrene_20)
LV_FONT_DECLARE(font_inter_20);

// Inset from the panel edge. The 2.16's glass has rounded corners; the rest of
// the UI clears them with a 20px margin, and the status row sits nearer the
// curve than anything else does, so it gets a wider one.
#define SB_PAD_X 34

// Where the glass pill sits: a thin iOS-style lozenge behind the status row,
// inset a few px from the panel edges so its rim catches light clearly.
// The panel's physical rounded corners cut any content near the top corners;
// the pill is inset far enough that its rim clears the curve entirely.
#define SB_PILL_PAD_X   10
#define SB_PILL_PAD_TOP 3

static lv_obj_t* bar        = nullptr;
static lv_obj_t* pill       = nullptr;
static lv_obj_t* lbl_clock  = nullptr;
static lv_obj_t* lbl_link_gh = nullptr;   // the ghost shadows (the legibility)
static lv_obj_t* lbl_batt_gh = nullptr;
static lv_obj_t* clk_ghost   = nullptr;
static constexpr int GHOST_DY = 2;   // the text shadow's drop (px)
static lv_obj_t* lbl_link   = nullptr;   // BLE + WiFi glyphs
static lv_obj_t* lbl_batt   = nullptr;

static ble_state_t s_ble       = BLE_STATE_INIT;
static bool        s_wifi_up   = false;
static int         s_batt_pct  = -1;
static bool        s_charging  = false;
static bool        s_immersive = false;
static bool        s_visible   = true;
static char        s_last_clock[12] = "";

static void apply_visibility(void) {
    if (!bar) return;
    if (s_visible && !s_immersive) lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
    else                           lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_link(void) {
    if (!lbl_link) return;
    // Both glyphs share one label so the row stays compact and right-aligned
    // without a layout pass. Disconnected states render dim rather than
    // vanishing, so the row never reflows as links come and go.
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%s",
             s_wifi_up ? PH_WIFI " " : "",
             s_ble == BLE_STATE_CONNECTED ? PH_BLUETOOTH : "");
    lv_label_set_text(lbl_link, buf);
    lv_obj_set_style_text_color(lbl_link, lv_color_hex(0xE5E5EA), 0);
    if (lbl_link_gh) {
        lv_label_set_text(lbl_link_gh, buf);
        lv_obj_align_to(lbl_link_gh, lbl_batt_gh, LV_ALIGN_OUT_LEFT_MID, -8, GHOST_DY);
    }
}

static void refresh_battery(void) {
    if (!lbl_batt) return;
    const char* glyph;
    if (s_charging)            glyph = PH_BATTERY_CHARGE;
    else if (s_batt_pct < 0)   glyph = PH_BATTERY_EMPTY;
    else if (s_batt_pct <= 10) glyph = PH_BATTERY_EMPTY;
    else if (s_batt_pct <= 35) glyph = PH_BATTERY_LOW;
    else if (s_batt_pct <= 65) glyph = PH_BATTERY_MED;
    else if (s_batt_pct <= 90) glyph = PH_BATTERY_HIGH;
    else                       glyph = PH_BATTERY_FULL;

    char buf[24];
    if (s_batt_pct >= 0) snprintf(buf, sizeof(buf), "%d%% %s", s_batt_pct, glyph);
    else                 snprintf(buf, sizeof(buf), "%s", glyph);
    lv_label_set_text(lbl_batt, buf);
    lv_obj_set_style_text_color(lbl_batt,
        (!s_charging && s_batt_pct >= 0 && s_batt_pct <= 10) ? THEME_RED
            : lv_color_hex(0xF2F2F7), 0);
    if (lbl_batt_gh) {
        lv_label_set_text(lbl_batt_gh, buf);
        lv_obj_align(lbl_batt_gh, LV_ALIGN_RIGHT_MID, -(SB_PAD_X + 14), GHOST_DY);
    }
}

void statusbar_init(lv_obj_t* parent) {
    const int W = board_caps().width;

    bar = lv_obj_create(parent);
    lv_obj_set_size(bar, W, STATUSBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    // The bar is drawn over app content but must never steal touches from it.
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    // Glass pill behind the status row — the same translucent treatment the
    // launcher tiles get, scaled to a lozenge: cool light on top fading to a
    // darker pool, thin bright rim on the edge, no shadow (it hangs in air
    // against the wallpaper, nothing to ground it onto).
    pill = lv_obj_create(bar);
    const int pill_w = W - 2 * SB_PILL_PAD_X;
    const int pill_h = STATUSBAR_H - 2 * SB_PILL_PAD_TOP;
    lv_obj_set_size(pill, pill_w, pill_h);
    lv_obj_set_pos(pill, SB_PILL_PAD_X, SB_PILL_PAD_TOP);
    lv_obj_set_style_radius(pill, pill_h / 2, 0);
    // Transmission recipe (critic r2): cream @low-opa over the wallpaper's
    // own color so the backlight shows through (was solid cream @175).
    // Photo-wall backdrop: the pill needs real opacity to read (the glass
    // 130 was tuned for the dark duotone; over a photo it turned mushy).
    // iOS-home status treatment: a deep neutral scrim, near-opaque.
    lv_obj_set_style_bg_color(pill, lv_color_mix(THEME_BG_BOT, lv_color_hex(0x000000), 55), 0);
    lv_obj_set_style_bg_opa(pill, 215, 0);
    lv_obj_set_style_bg_grad_color(pill, lv_color_mix(THEME_BG_BOT, lv_color_hex(0x000000), 25), 0);
    lv_obj_set_style_bg_grad_opa(pill, 235, 0);
    lv_obj_set_style_bg_grad_dir(pill, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(pill, 0, 0);
    lv_obj_set_style_bg_grad_stop(pill, 255, 0);
    // Asymmetric liquid specular (r8: near-uniform hairline = frosted class;
    // Apple's pill has a BRIGHTER top rim): border stays warm cream but a
    // clipped bright top-slab child adds the directional specular segment.
    lv_obj_set_style_border_color(pill, RIM_CREAM, 0);
    lv_obj_set_style_border_width(pill, 2, 0);
    lv_obj_set_style_border_opa(pill, 150, 0);
    lv_obj_set_style_outline_color(pill, GLASS_SHADOW, 0);
    lv_obj_set_style_outline_width(pill, 1, 0);
    lv_obj_set_style_outline_pad(pill, 0, 0);
    lv_obj_set_style_outline_opa(pill, 80, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_IGNORE_LAYOUT);

    // Top sheen on the pill: the same light-wash the tiles wear, shortened so
    // it stays in the pill's upper half and reads as one material with them.
    lv_obj_t* sheen = lv_obj_create(pill);
    lv_obj_set_size(sheen, pill_w, pill_h / 2);
    lv_obj_set_pos(sheen, 0, 0);
    lv_obj_set_style_bg_color(sheen, RIM_HI, 0);
    lv_obj_set_style_bg_grad_color(sheen, RIM_HI, 0);
    lv_obj_set_style_bg_grad_opa(sheen, 0, 0);
    lv_obj_set_style_bg_grad_dir(sheen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(sheen, 52, 0);
    lv_obj_set_style_radius(sheen, pill_h / 2, 0);
    lv_obj_set_style_border_width(sheen, 0, 0);
    lv_obj_set_style_pad_all(sheen, 0, 0);
    lv_obj_clear_flag(sheen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sheen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sheen, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_clip_corner(sheen, true, 0);

    // Interior refraction streak (r8 design gap): a soft warm amber pool in
    // the fill's lower-left, 0-opa fading — reads as light refracted inside
    // the slab, not a paint stripe.
    lv_obj_t* pool = lv_obj_create(pill);
    lv_obj_set_size(pool, (pill_w * 3) / 5, (pill_h * 2) / 3);
    lv_obj_set_pos(pool, 0, pill_h / 3);
    lv_obj_set_style_radius(pool, pill_h, 0);
    lv_obj_set_style_bg_color(pool, lv_color_hex(0xd98e5a), 0);
    lv_obj_set_style_bg_grad_color(pool, lv_color_hex(0xd98e5a), 0);
    lv_obj_set_style_bg_opa(pool, 26, 0);
    lv_obj_set_style_bg_grad_opa(pool, 0, 0);
    lv_obj_set_style_bg_grad_dir(pool, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(pool, 0, 0);
    lv_obj_set_style_bg_grad_stop(pool, 255, 0);
    lv_obj_set_style_border_width(pool, 0, 0);
    lv_obj_set_style_pad_all(pool, 0, 0);
    lv_obj_clear_flag(pool, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pool, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(pool, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_clip_corner(pool, true, 0);

    lbl_clock = lv_label_create(bar);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_set_style_text_font(lbl_clock, &font_inter_20, 0);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0xffffff), 0);
    lv_obj_align(lbl_clock, LV_ALIGN_LEFT_MID, SB_PAD_X, 0);

    lbl_batt = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_batt, &font_inter_20, 0);
    lv_obj_set_style_text_color(lbl_batt, lv_color_hex(0xF2F2F7), 0);
    lbl_batt_gh = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_batt_gh, &font_inter_20, 0);
    lv_obj_set_style_text_color(lbl_batt_gh, lv_color_hex(0x0a0f14), 0);
    lv_obj_set_style_text_opa(lbl_batt_gh, 140, 0);
    lv_obj_align(lbl_batt_gh, LV_ALIGN_RIGHT_MID, -(SB_PAD_X + 14), GHOST_DY);
    // Rounded-corner clearance: at the bar's height the corner curve eats
    // everything past ~x446; the battery row gets a deeper inset than the
    // clock's so the glyph's cap survives it.
    lv_obj_align(lbl_batt, LV_ALIGN_RIGHT_MID, -(SB_PAD_X + 14), 0);

    lbl_link = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_link, &font_inter_20, 0);
    lv_obj_set_style_text_color(lbl_link, lv_color_hex(0xE5E5EA), 0);
    lv_obj_align_to(lbl_link, lbl_batt, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lbl_link_gh = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_link_gh, &font_inter_20, 0);
    lv_obj_set_style_text_color(lbl_link_gh, lv_color_hex(0x0a0f14), 0);
    lv_obj_set_style_text_opa(lbl_link_gh, 140, 0);

    if (!board_caps().has_battery) {
        lv_obj_add_flag(lbl_batt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(lbl_link, LV_ALIGN_RIGHT_MID, -SB_PAD_X, 0);
    }

    refresh_link();
    refresh_battery();
    // The ghosts mirror their live labels (one relayout point).
    if (clk_ghost) {
        lv_label_set_text(clk_ghost, lv_label_get_text(lbl_clock));
    }
}

void statusbar_tick(void) {
    if (!lbl_clock || !s_visible || s_immersive) return;
    char buf[12];
    clock_src_format(buf, sizeof(buf));
    if (strcmp(buf, s_last_clock) != 0) {   // only relayout when the minute flips
        strlcpy(s_last_clock, buf, sizeof(s_last_clock));
        lv_label_set_text(lbl_clock, buf);
    }
}

void statusbar_set_visible(bool visible) {
    s_visible = visible;
    apply_visibility();
}

void statusbar_set_immersive(bool immersive) {
    s_immersive = immersive;
    apply_visibility();
}

void statusbar_set_battery(int percent, bool charging) {
    s_batt_pct = percent;
    s_charging = charging;
    refresh_battery();
}

void statusbar_set_ble(ble_state_t state) {
    s_ble = state;
    refresh_link();
}

void statusbar_set_wifi(bool up, int rssi) {
    (void)rssi;   // reserved: bar strength once the net layer reports it
    s_wifi_up = up;
    refresh_link();
}
