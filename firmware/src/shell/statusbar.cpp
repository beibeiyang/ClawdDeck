#include "statusbar.h"
#include "phosphor_cp.h"
#include "clock_src.h"
#include "../theme.h"
#include "../hal/board_caps.h"
#include <stdio.h>
#include <string.h>

// Apple-grade status type: Inter Medium (SIL OFL) — 32px: the photo wall
// drowned the 28px row; bigger + a top scrim = the legibility fix.
LV_FONT_DECLARE(font_inter_32);

// Inset from the panel edge. The 2.16's glass has rounded corners; the rest of
// the UI clears them with a 20px margin, and the status row sits nearer the
// curve than anything else does, so it gets a wider one.
#define SB_PAD_X 34


// The panel's physical rounded corners cut any content near the top corners;
// the status text insets clear the curve entirely.
#define SB_PILL_PAD_X   10
#define SB_PILL_PAD_TOP 3

static lv_obj_t* bar        = nullptr;
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
    if (lbl_link_gh) lv_label_set_text(lbl_link_gh, buf);
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

// The top-fade scrim's ramp: 80% black at the top edge → clear at the row's
// bottom; LVGL's stop fracs = 0..255.
static const lv_color_t scrim_cols[2] = { LV_COLOR_MAKE(0,0,0), LV_COLOR_MAKE(0,0,0) };
static const uint8_t     scrim_opas[2] = { LV_OPA_80, LV_OPA_TRANSP };
static const uint8_t     scrim_fracs[2] = { 0, 255 };

void statusbar_init(lv_obj_t* parent) {
    const int W = board_caps().width;

    bar = lv_obj_create(parent);
    lv_obj_set_size(bar, W, STATUSBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    // Legibility over the photo: a short TOP SCRIM — black fading to clear
    // within ~1 row height (not a glass box: the = = the = = the factory
    // anatomy carries no pill and the = = the = = the = = = =
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_opa(bar, 255, 0);
    static lv_grad_dsc_t scrim_grad;
    lv_grad_init_stops(&scrim_grad, scrim_cols, scrim_opas, scrim_fracs, 2);
    lv_grad_vertical_init(&scrim_grad);
    lv_obj_set_style_bg_grad(bar, &scrim_grad, 0);
    lv_obj_set_style_bg_grad_opa(bar, 255, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    // The bar is drawn over app content but must never steal touches from it.
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    // The reference desktop carries NO box behind the status row: white
    // text + glyphs directly over the wallpaper, legibility via a 2px dark
    // ghost under each item (the iOS/factory anatomy). The old glass pill
    // smeared over the photo — cut entirely per user call.
    lbl_clock = lv_label_create(bar);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_set_style_text_font(lbl_clock, &font_inter_32, 0);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0xffffff), 0);
    lv_obj_align(lbl_clock, LV_ALIGN_LEFT_MID, SB_PAD_X, 0);
    clk_ghost = lv_label_create(bar);
    lv_label_set_text(clk_ghost, "--:--");
    lv_obj_set_style_text_font(clk_ghost, &font_inter_32, 0);
    lv_obj_set_style_text_color(clk_ghost, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_opa(clk_ghost, 190, 0);
    lv_obj_align(clk_ghost, LV_ALIGN_LEFT_MID, SB_PAD_X - 2, GHOST_DY);

    lbl_batt = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_batt, &font_inter_32, 0);
    lv_obj_set_style_text_color(lbl_batt, lv_color_hex(0xffffff), 0);
    lbl_batt_gh = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_batt_gh, &font_inter_32, 0);
    lv_obj_set_style_text_color(lbl_batt_gh, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_opa(lbl_batt_gh, 190, 0);
    lv_obj_align(lbl_batt_gh, LV_ALIGN_RIGHT_MID, -(SB_PAD_X + 14 - 2), GHOST_DY);
    // Rounded-corner clearance: at the bar's height the corner curve eats
    // everything past ~x446; the battery row gets a deeper inset than the
    // clock's so the glyph's cap survives it.
    lv_obj_align(lbl_batt, LV_ALIGN_RIGHT_MID, -(SB_PAD_X + 14), 0);

    lbl_link = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_link, &font_inter_32, 0);
    lv_obj_set_style_text_color(lbl_link, lv_color_hex(0xffffff), 0);
    lv_obj_align_to(lbl_link, lbl_batt, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lbl_link_gh = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl_link_gh, &font_inter_32, 0);
    lv_obj_set_style_text_color(lbl_link_gh, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_opa(lbl_link_gh, 190, 0);
    lv_obj_align_to(lbl_link_gh, lbl_batt_gh, LV_ALIGN_OUT_LEFT_MID, -8, GHOST_DY);

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
    if (lbl_batt_gh) {
        lv_label_set_text(lbl_batt_gh, lv_label_get_text(lbl_batt));
        lv_obj_align(lbl_batt_gh, LV_ALIGN_RIGHT_MID, -(SB_PAD_X + 14 - 2), GHOST_DY);
    }
}

void statusbar_tick(void) {
    if (!lbl_clock || !s_visible || s_immersive) return;
    char buf[12];
    clock_src_format(buf, sizeof(buf));
    if (strcmp(buf, s_last_clock) != 0) {   // only relayout when the minute flips
        strlcpy(s_last_clock, buf, sizeof(s_last_clock));
        lv_label_set_text(lbl_clock, buf);
        if (clk_ghost) lv_label_set_text(clk_ghost, buf);
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
