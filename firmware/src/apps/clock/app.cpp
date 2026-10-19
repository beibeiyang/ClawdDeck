#include "../../shell/app.h"
#include "misc/lv_text_private.h"
#include "../../shell/clock_src.h"
#include "../../theme.h"
#include "../../phosphor_cp.h"
#include "../../hal/board_caps.h"
#include "../../ble.h"

LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_inter_18);
LV_FONT_DECLARE(font_inter_24);
LV_FONT_DECLARE(font_inter_16);

// Typography-first wall clock, fed by the host daemon's wall-clock fields.

static lv_obj_t* time_lbl = nullptr;
static lv_obj_t* sec_lbl  = nullptr;
static lv_obj_t* date_lbl = nullptr;
static lv_obj_t* hint_lbl = nullptr;
static lv_obj_t* sync_dot = nullptr;
static uint32_t    last_tick = 0;

static void refresh(void) {
    char formatted[32];
    clock_src_format_hms(formatted, sizeof(formatted));
    char main[16] = "--:--";
    char seconds[12] = "";
    const char* last_colon = strrchr(formatted, ':');
    if (last_colon) {
        const size_t main_len = (size_t)(last_colon - formatted);
        if (main_len < sizeof(main)) {
            memcpy(main, formatted, main_len);
            main[main_len] = '\0';
            snprintf(seconds, sizeof(seconds), ":%s", last_colon + 1);
        }
    } else {
        snprintf(main, sizeof(main), "%s", formatted);
    }
    if (time_lbl) lv_label_set_text(time_lbl, main);
    if (sec_lbl) lv_label_set_text(sec_lbl, seconds);

    if (time_lbl && sec_lbl) {
        const bool compact = board_caps().width < 400;
        const lv_font_t* main_font = compact ? &font_tiempos_34 : &font_tiempos_56;
        const lv_font_t* seconds_font = compact ? &font_inter_18 : &font_inter_24;
        lv_point_t main_size = {};
        lv_point_t seconds_size = {};
        lv_text_get_size(&main_size, main, main_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_text_get_size(&seconds_size, seconds, seconds_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

        const int gap = seconds[0] ? (compact ? 4 : 7) : 0;
        const int group_width = main_size.x + gap + seconds_size.x;
        const int group_x = (board_caps().width - group_width) / 2;
        const int main_y = compact ? 174 : 171;
        lv_obj_set_pos(time_lbl, group_x, main_y);
        lv_obj_set_pos(sec_lbl, group_x + main_size.x + gap,
                       main_y + (compact ? 20 : 30));
    }

    char date[32];
    clock_src_format_date(date, sizeof(date));
    if (date_lbl) lv_label_set_text(date_lbl, date);

    const bool synced = clock_src_valid() && ble_get_state() == BLE_STATE_CONNECTED;
    if (sync_dot) {
        lv_obj_set_style_bg_color(sync_dot,
            synced ? THEME_GREEN : lv_color_hex(0x74787d), 0);
    }
    if (hint_lbl) {
        lv_label_set_text(hint_lbl, synced ? "Synced from Mac" : "Waiting for Mac");
        lv_obj_set_style_text_color(hint_lbl,
            synced ? THEME_GREEN : THEME_DIM, 0);
    }
}

static void clock_create(lv_obj_t* root) {
    const int W = board_caps().width;
    const int H = board_caps().height;
    const bool compact = (W < 400);

    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    // Quiet status-bar separation and one typography-led time hierarchy.
    hint_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(hint_lbl, &font_inter_16, 0);
    lv_obj_set_style_text_align(hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint_lbl, W);
    lv_obj_set_pos(hint_lbl, 0, H - 83);
    lv_label_set_long_mode(hint_lbl, LV_LABEL_LONG_CLIP);

    time_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(time_lbl,
                               compact ? &font_tiempos_34 : &font_tiempos_56, 0);
    lv_obj_set_style_text_color(time_lbl, THEME_TEXT, 0);
    lv_label_set_long_mode(time_lbl, LV_LABEL_LONG_CLIP);

    sec_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(sec_lbl,
                               compact ? &font_inter_18 : &font_inter_24, 0);
    lv_obj_set_style_text_color(sec_lbl, THEME_DIM, 0);
    lv_label_set_long_mode(sec_lbl, LV_LABEL_LONG_CLIP);

    date_lbl = lv_label_create(root);
    lv_obj_set_width(date_lbl, W - 32);
    lv_obj_set_style_text_font(date_lbl, compact ? &font_inter_18 : &font_inter_24, 0);
    lv_obj_set_style_text_color(date_lbl, THEME_DIM, 0);
    lv_obj_set_style_text_align(date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(date_lbl, 16, compact ? 245 : 250);
    lv_label_set_long_mode(date_lbl, LV_LABEL_LONG_CLIP);

    sync_dot = lv_obj_create(root);
    lv_obj_remove_style_all(sync_dot);
    lv_obj_set_size(sync_dot, 7, 7);
    lv_obj_set_style_radius(sync_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sync_dot, lv_color_hex(0x74787d), 0);
    lv_obj_set_style_bg_opa(sync_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sync_dot, 0, 0);
    lv_obj_set_pos(sync_dot, W / 2 - (compact ? 59 : 72), H - 74);
    lv_obj_clear_flag(sync_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sync_dot, LV_OBJ_FLAG_CLICKABLE);

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
    sec_lbl  = nullptr;
    date_lbl = nullptr;
    hint_lbl = nullptr;
    sync_dot = nullptr;
}

static void clock_on_ble(ble_state_t) {
    refresh();
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
    .on_ble        = clock_on_ble,
    .on_battery    = nullptr,
};
