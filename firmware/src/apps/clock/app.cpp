#include "../../shell/app.h"
#include "../../shell/clock_src.h"
#include "../../shell/statusbar.h"
#include "../../theme.h"
#include "../../phosphor_cp.h"
#include "../../hal/board_caps.h"

LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_14);

// Full-screen clock fed by the host daemon's wall-clock fields (t + tf).
// No onboard RTC — the Mac daemon stamps every payload with local time.

#define CLOCK_FOOT_H 34

static lv_obj_t* time_lbl = nullptr;
static lv_obj_t* date_lbl = nullptr;
static lv_obj_t* hint_lbl = nullptr;
static uint32_t    last_tick = 0;

static void refresh(void) {
    char buf[32];
    clock_src_format_hms(buf, sizeof(buf));
    if (time_lbl) lv_label_set_text(time_lbl, buf);

    clock_src_format_date(buf, sizeof(buf));
    if (date_lbl) lv_label_set_text(date_lbl, buf);

    if (hint_lbl) {
        if (clock_src_valid()) {
            lv_label_set_text(hint_lbl,
                              clock_src_fmt() == 12 ? "12-hour · from Mac"
                                                    : "24-hour · from Mac");
        } else {
            lv_label_set_text(hint_lbl, "Connect ClawdDeck to your Mac for time");
        }
    }
}

static void clock_create(lv_obj_t* root) {
    const int W = board_caps().width;
    const bool compact = (W < 400);

    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    time_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(time_lbl,
                               compact ? &font_tiempos_34 : &font_tiempos_56, 0);
    lv_obj_set_style_text_color(time_lbl, THEME_TEXT, 0);
    lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, STATUSBAR_H / 2 - 20);

    date_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(date_lbl, &font_styrene_20, 0);
    lv_obj_set_style_text_color(date_lbl, THEME_DIM, 0);
    lv_obj_align_to(date_lbl, time_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, compact ? 12 : 20);

    hint_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(hint_lbl, &font_styrene_14, 0);
    lv_obj_set_style_text_color(hint_lbl, THEME_ACCENT, 0);
    lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -(CLOCK_FOOT_H + 16));

    refresh();
    last_tick = lv_tick_get();
}

static void clock_tick(void) {
    // Redraw once per second for the seconds hand; also picks up the first
    // daemon payload after connect without waiting for a minute rollover.
    if (lv_tick_elaps(last_tick) < 1000) return;
    last_tick = lv_tick_get();
    refresh();
}

static void clock_destroy(void) {
    time_lbl = nullptr;
    date_lbl = nullptr;
    hint_lbl = nullptr;
}

extern const AppDef app_clock = {
    .id            = "clock",
    .title         = "Clock",
    .glyph         = PH_CLOCK,
    .tile_rgb      = 0x2b5b6e,
    .blurb         = "Wall clock synced from your Mac while ClawdDeck is paired.",
    .required_caps = APP_CAP_NONE,
    .persistent    = false,
    .immersive     = false,
    .create        = clock_create,
    .destroy       = clock_destroy,
    .tick          = clock_tick,
    .on_button     = nullptr,
    .on_usage      = nullptr,
    .on_ble        = nullptr,
    .on_battery    = nullptr,
};
