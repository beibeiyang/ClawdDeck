#include "statusbar.h"
#include "clock_src.h"
#include "../theme.h"
#include "../hal/board_caps.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_styrene_20);

// Inset from the panel edge. The 2.16's glass has rounded corners; the rest of
// the UI clears them with a 20px margin, and the status row sits nearer the
// curve than anything else does, so it gets a wider one.
#define SB_PAD_X 34

static lv_obj_t* bar        = nullptr;
static lv_obj_t* lbl_clock  = nullptr;
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
             s_wifi_up ? LV_SYMBOL_WIFI " " : "",
             s_ble == BLE_STATE_CONNECTED ? LV_SYMBOL_BLUETOOTH : "");
    lv_label_set_text(lbl_link, buf);
    lv_obj_set_style_text_color(lbl_link,
        (s_ble == BLE_STATE_CONNECTED || s_wifi_up) ? THEME_TEXT : THEME_DIM, 0);
}

static void refresh_battery(void) {
    if (!lbl_batt) return;
    const char* glyph;
    if (s_charging)        glyph = LV_SYMBOL_CHARGE;
    else if (s_batt_pct < 0)  glyph = LV_SYMBOL_BATTERY_EMPTY;
    else if (s_batt_pct <= 10) glyph = LV_SYMBOL_BATTERY_EMPTY;
    else if (s_batt_pct <= 35) glyph = LV_SYMBOL_BATTERY_1;
    else if (s_batt_pct <= 65) glyph = LV_SYMBOL_BATTERY_2;
    else if (s_batt_pct <= 90) glyph = LV_SYMBOL_BATTERY_3;
    else                       glyph = LV_SYMBOL_BATTERY_FULL;

    char buf[24];
    if (s_batt_pct >= 0) snprintf(buf, sizeof(buf), "%d%% %s", s_batt_pct, glyph);
    else                 snprintf(buf, sizeof(buf), "%s", glyph);
    lv_label_set_text(lbl_batt, buf);
    lv_obj_set_style_text_color(lbl_batt,
        (!s_charging && s_batt_pct >= 0 && s_batt_pct <= 10) ? THEME_RED : THEME_DIM, 0);
}

void statusbar_init(lv_obj_t* parent) {
    const int W = board_caps().width;

    bar = lv_obj_create(parent);
    lv_obj_set_size(bar, W, STATUSBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    // The bar is drawn over app content but must never steal touches from it.
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    lbl_clock = lv_label_create(bar);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_set_style_text_font(lbl_clock, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_clock, THEME_TEXT, 0);
    lv_obj_align(lbl_clock, LV_ALIGN_LEFT_MID, SB_PAD_X, 0);

    lbl_batt = lv_label_create(bar);
    lv_obj_set_style_text_color(lbl_batt, THEME_DIM, 0);
    lv_obj_align(lbl_batt, LV_ALIGN_RIGHT_MID, -SB_PAD_X, 0);

    lbl_link = lv_label_create(bar);
    lv_obj_set_style_text_color(lbl_link, THEME_DIM, 0);
    lv_obj_align_to(lbl_link, lbl_batt, LV_ALIGN_OUT_LEFT_MID, -8, 0);

    if (!board_caps().has_battery) {
        lv_obj_add_flag(lbl_batt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(lbl_link, LV_ALIGN_RIGHT_MID, -SB_PAD_X, 0);
    }

    refresh_link();
    refresh_battery();
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
