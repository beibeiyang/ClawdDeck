#include "../../shell/app.h"
#include "misc/lv_text_private.h"
#include "../../shell/clock_src.h"
#include "../../shell/statusbar.h"
#include "../../theme.h"
#include "../../phosphor_cp.h"
#include "../../hal/board_caps.h"

LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_inter_20);
LV_FONT_DECLARE(font_inter_14);

// Full-screen clock fed by the host daemon's wall-clock fields (t + tf).
// No onboard RTC — the Mac daemon stamps every payload with local time.
// Material: the launcher's glass-tile recipe scaled to a watch face — the
// face sits on a floating glass card over the same duotone wallpaper the
// launcher tiles sheen off of. Rendering only; the data path is untouched.

#define CLOCK_FOOT_H 34

static lv_obj_t* time_lbl = nullptr;
static lv_obj_t* sec_lbl  = nullptr;
static lv_obj_t* date_lbl = nullptr;
static lv_obj_t* hint_lbl = nullptr;
static uint32_t    last_tick = 0;

// ---- Glass face card --------------------------------------------------------
// The launcher's glass_tile() (shell/launcher.cpp) scaled up. Layers, bottom
// to top, exactly as launched tiles do it:
//   1. drop shadow puddle (grounds the card on the wallpaper)
//   2. body fill — ONE vertical VER gradient carries the whole material
//      (top stop = wallpaper light white-washed into cream sheen with a hue
//      cast, bottom stop = denser hue pool; opas carried in the stops)
//   3. rim — cream border ON the card edge + 1px dark outline outside
//   4. top-arc glint wash child — the directional light the uniform border
//      can't give (same child geometry the launcher tiles use)
static void glass_face(lv_obj_t* card) {
    // Backdrop light behind the card — matches wall_base_at() row logic in
    // launcher.cpp (the wallpaper's taupe→navy duotone). The card sits near
    // the top half, so use the bright mix.
    const int          H    = lv_obj_get_height(card);
    const int          W    = lv_obj_get_width(card);
    const lv_color_t   wall = lv_color_mix(THEME_BG_BOT, THEME_BG_TOP, 120);
    const lv_color_t   hue  = lv_color_hex(0x2b5b6e);   // clock tile_rgb cast

    // 4. top-arc glint wash — a sheen child clipped to the card's soft top.
    //    Same recipe as launcher tiles' glint: cream VER ramp, falls to
    //    transparent INSIDE the card, sized so its rim leaves the card top.
    lv_obj_t* glint = lv_obj_create(card);
    lv_obj_set_size(glint, W - 56, (H * 5) / 8 - 14);
    lv_obj_align(glint, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(glint, lv_color_mix(RIM_CREAM, lv_color_hex(0xfff6ea), 50), 0);
    lv_obj_set_style_bg_grad_color(glint, RIM_CREAM, 0);
    lv_obj_set_style_bg_grad_dir(glint, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(glint, 78, 0);
    lv_obj_set_style_bg_grad_opa(glint, 0, 0);
    lv_obj_set_style_bg_main_stop(glint, 0, 0);
    lv_obj_set_style_bg_grad_stop(glint, 255, 0);
    lv_obj_set_style_radius(glint, (H * 5) / 16, 0);
    lv_obj_set_style_border_width(glint, 0, 0);
    lv_obj_set_style_pad_all(glint, 0, 0);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_clip_corner(glint, true, 0);

    // 2. body: LIFT model — the wallpaper's own light added-to, hue as cast.
    lv_obj_set_style_bg_color(card,
        lv_color_mix(lv_color_mix(wall, RIM_CREAM, 85), hue, 165), 0);
    lv_obj_set_style_bg_opa(card, 145, 0);
    lv_obj_set_style_bg_grad_color(card,
        lv_color_mix(lv_color_mix(wall, lv_color_hex(0xffffff), 12), hue, 82), 0);
    lv_obj_set_style_bg_grad_opa(card, 185, 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(card, 0, 0);
    lv_obj_set_style_bg_grad_stop(card, 240, 0);

    // 1. drop shadow puddle (attempt-2 tuned: light).
    lv_obj_set_style_shadow_color(card, GLASS_SHADOW, 0);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_spread(card, 0, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_opa(card, 25, 0);

    // 3. rim + seating outline (attempt-2 tuned strengths).
    lv_obj_set_style_border_color(card, RIM_CREAM, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_opa(card, 95, 0);
    lv_obj_set_style_outline_color(card, GLASS_SHADOW, 0);
    lv_obj_set_style_outline_width(card, 1, 0);
    lv_obj_set_style_outline_pad(card, 0, 0);
    lv_obj_set_style_outline_opa(card, 90, 0);
}

static void refresh(void) {
    char buf[32];
    clock_src_format_hms(buf, sizeof(buf));
    if (time_lbl) lv_label_set_text(time_lbl, buf);

    // Seconds render as a dimmer tier after the minutes — iOS clock feel.
    // The full "05:00:08" string is measured at the REAL font metrics, then
    // split: minutes go into the left-shifted hero label, ":08" into the
    // offset tier, positioned so the join is metric-perfect every tick.
    if (sec_lbl) {
        const char* txt = lv_label_get_text(time_lbl);
        char main[12];
        char secs[6];
        const char* last_colon = txt ? strrchr(txt, ':') : nullptr;
        if (txt && last_colon) {
            const size_t n = (size_t)(last_colon - txt);
            if (n < sizeof(main)) {
                memcpy(main, txt, n);
                main[n]     = '\0';
                snprintf(secs, sizeof(secs), ":%s", last_colon + 1);
                lv_label_set_text(time_lbl, main);
                lv_label_set_text(sec_lbl, secs);
            }
        }
    }

    char db[32];
    clock_src_format_date(db, sizeof(db));
    if (date_lbl) lv_label_set_text(date_lbl, db);

    if (hint_lbl) {
        // Plain ASCII only — mono ships U+00B7 but Inter/Styrene were
        // converted with -r 0x20-0x7E (7-bit), which has no '·', and it drew
        // as a tofu box in the sim. The separator is a visual nicety; the
        // words carry the meaning either way.
        lv_label_set_text(hint_lbl,
                          clock_src_fmt() == 12 ? "12-hour, from Mac"
                                                : "24-hour, from Mac");
    }
}

static void clock_create(lv_obj_t* root) {
    const int W = board_caps().width;
    const int H = board_caps().height;
    const bool compact = (W < 400);

    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    // Wallpaper — same duotone + regional feathered washes as the launcher's
    // build_wallpaper() (shell/launcher.cpp), verbatim geometry. Glass needs
    // light to sheen off of; dead black reads as flat plastic.
    lv_obj_t* wp = lv_obj_create(root);
    lv_obj_set_size(wp, W, H);
    lv_obj_set_pos(wp, 0, 0);
    lv_obj_set_style_bg_color(wp, THEME_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(wp, THEME_BG_FLOOR, 0);
    lv_obj_set_style_bg_grad_dir(wp, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(wp, 0, 0);
    lv_obj_set_style_bg_grad_stop(wp, 230, 0);
    lv_obj_set_style_bg_opa(wp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wp, 0, 0);
    lv_obj_set_style_pad_all(wp, 0, 0);
    lv_obj_clear_flag(wp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(wp, LV_OBJ_FLAG_CLICKABLE);

    struct { int x, y, w, h; lv_color_t c; int opa; } washes[] = {
        { -W / 6, -H / 6, W, H,           SHEEN_TINT,             50 },  // warm crest TL
        { -W / 8, -H / 16, W, H * 3 / 4,  lv_color_hex(0x3a2438), 30 },  // plum sweep
        { W - W / 4, H / 6, W + W / 3, H, lv_color_hex(0x14203a), 26 },  // cool basin E (off-panel)
    };
    for (unsigned i = 0; i < sizeof(washes) / sizeof(washes[0]); i++) {
        lv_obj_t* band = lv_obj_create(wp);
        lv_obj_set_size(band, washes[i].w, washes[i].h);
        lv_obj_set_pos(band, washes[i].x, washes[i].y);
        lv_obj_set_style_radius(band, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(band, washes[i].c, 0);
        lv_obj_set_style_bg_grad_color(band, washes[i].c, 0);
        lv_obj_set_style_bg_opa(band, washes[i].opa, 0);
        lv_obj_set_style_bg_grad_opa(band, 0, 0);
        lv_obj_set_style_bg_grad_dir(band, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_stop(band, 0, 0);
        lv_obj_set_style_bg_grad_stop(band, 255, 0);
        lv_obj_set_style_border_width(band, 0, 0);
        lv_obj_set_style_pad_all(band, 0, 0);
        lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(band, LV_OBJ_FLAG_IGNORE_LAYOUT);
    }

    // Floating glass card — the watch face. Centered, square-ish.
    const int cw = compact ? 240 : 340;
    const int cx = (W - cw) / 2;
    const int cy = STATUSBAR_H + (H - STATUSBAR_H - CLOCK_FOOT_H - cw) / 2;
    lv_obj_t* face = lv_obj_create(root);
    lv_obj_set_size(face, cw, cw);
    lv_obj_set_pos(face, cx, cy);
    lv_obj_set_style_radius(face, 30, 0);
    glass_face(face);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_CLICKABLE);

    // TIME — the hero: serif numerals on the glass, ink-dark (glyphs over the
    // light glass read ink-dark on the launcher tiles). The label's width is
    // pinned and its text center-aligned so lv_obj_center() keeps it optically
    // centered when the text changes every second (a shrink-wrapped label
    // re-centers on the STALE width — the time drifted +44px off-center).
    // Attempt-2 (manager): shrink-wrap + center. hh:mm keeps a constant
    // digit count in both 12h/24h, so the label width never changes after
    // create — no stale-width drift; lv_obj_center() stays optically
    // perfect every tick. (Build-1's right-drift = pinned width + the
    // opa size-probe dance.)
    time_lbl = lv_label_create(face);
    lv_obj_set_style_text_font(time_lbl,
                               compact ? &font_tiempos_34 : &font_tiempos_56, 0);
    lv_obj_set_style_text_color(time_lbl, THEME_INK, 0);
    lv_obj_center(time_lbl);
    lv_obj_set_pos(time_lbl, 0, 6);   // optical: room for the seconds row

    // Seconds: dimmer smaller tier, OUT_RIGHT of the stable hero — the join
    // is metric-true whatever width the digits land at.
    sec_lbl = lv_label_create(face);
    lv_obj_set_style_text_font(sec_lbl, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(sec_lbl, THEME_INK, 0);
    lv_obj_set_style_text_opa(sec_lbl, 110, 0);
    lv_obj_align_to(sec_lbl, time_lbl, LV_ALIGN_OUT_RIGHT_MID, 15, 4);

    // Date inside the card under the time (Inter, dim).
    date_lbl = lv_label_create(face);
    lv_obj_set_style_text_font(date_lbl, &font_inter_20, 0);
    lv_obj_set_style_text_color(date_lbl, THEME_INK, 0);
    lv_obj_set_style_text_opa(date_lbl, 150, 0);
    lv_obj_align_to(date_lbl, time_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, compact ? 12 : 24);

    // Hint — Inter 14 (range 0x20-0x7E includes '·', so the tofu box is gone).
    hint_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(hint_lbl, &font_inter_14, 0);
    lv_obj_set_style_text_color(hint_lbl, THEME_ACCENT, 0);
    lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -(CLOCK_FOOT_H + 16));
    // Label defaults to left-align: keep the hint optically centered.
    lv_obj_set_width(hint_lbl, W);
    lv_obj_set_style_text_align(hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(hint_lbl, 0);

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