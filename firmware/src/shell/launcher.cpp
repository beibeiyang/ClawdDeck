#include "launcher.h"
#include "shell.h"
#include "draw/snapshot/lv_snapshot.h"
#include "statusbar.h"
#include "../theme.h"
#include "../hal/board_caps.h"
#include "draw/snapshot/lv_snapshot.h"
#include <stdio.h>
#include <time.h>


LV_FONT_DECLARE(font_inter_28);
LV_FONT_DECLARE(font_inter_16);
LV_FONT_DECLARE(font_inter_14);
LV_FONT_DECLARE(font_phos_40);
LV_FONT_DECLARE(font_phos_28);
LV_FONT_DECLARE(font_phos_20);
LV_FONT_DECLARE(font_phos_fill_96);

#include <math.h>
#include <string.h>
#ifdef BOARD_SIM
#include "boards/sim/shim/esp_heap_caps.h"
#else
#include <esp_heap_caps.h>
#endif
LV_FONT_DECLARE(font_phos_fill_64);

// Space kept clear at the bottom for the page dots and the home pill.
#define LAUNCHER_FOOT_H 34

// Geometry derived from the panel size, in the same spirit as ui.cpp's
// compute_layout(): one place decides the breakpoints so the tile builder
// below never needs a board conditional. Verified against 480x480 (2.16),
// 368x448 (1.8) and 240x240 (1.54).
struct LauncherLayout {
    int cols, rows, per_page;
    int inset;          // left/right margin, also clears the rounded corners
    int grid_h;         // height of the tile area
    int row_gap;        // vertical gap between tile rows
    int cell_w, cell_h; // one tile's slot
    int tile;           // icon square edge
    int radius;         // squircle-ish corner radius
    int label_gap;
    const lv_font_t* glyph_font;
    const lv_font_t* label_font;
};
static LauncherLayout G;

static lv_obj_t* root     = nullptr;
static lv_obj_t* pages    = nullptr;
static lv_obj_t* dots_row = nullptr;
static int       page_count = 1;

static void compute_layout(const BoardCaps& c) {
    const int W = c.width;
    const int H = c.height;

    // Fixed 2x2 on every panel: four big tiles per page, paging for the rest.
    // A 3x3 grid fits, but at arm's length the tiles are small enough to
    // mis-hit, so we trade density for page count the way the stock Waveshare
    // launcher does.
    G.cols     = 2;
    G.rows     = 2;
    G.per_page = 4;
    G.inset    = (W >= 400) ? 34 : (W >= 300) ? 24 : 10;

    G.row_gap = (H >= 460) ? 12 : 6;
    G.grid_h  = H - STATUSBAR_H - LAUNCHER_FOOT_H;
    G.cell_w  = (W - 2 * G.inset) / G.cols;
    G.cell_h  = (G.grid_h - G.row_gap) / G.rows;

    // Leave room beside the tile for breathing space and beneath it for the
    // label; the smaller of the two axes wins so tiles stay square.
    const int by_w = G.cell_w - ((W >= 400) ? 44 : 20);
    const int by_h = G.cell_h - ((W >= 400) ? 52 : 34);
    G.tile = (by_w < by_h) ? by_w : by_h;
    if (G.tile < 36) G.tile = 36;

    // iOS squircles are ~22% of the icon edge; LVGL only has circular corners,
    // so a slightly larger radius reads closest at these sizes.
    G.radius    = G.tile * 28 / 100 + 2;
    G.label_gap = (G.tile >= 80) ? 10 : 4;

    // Launcher tiles = the iOS app-icon class: the Phosphor-FILL glyph family.
    G.glyph_font = (G.tile >= 150) ? &font_phos_fill_96
                 : (G.tile >=  88) ? &font_phos_fill_64
                 : (G.tile >=  60) ? &font_phos_28
                                   : &font_phos_20;
    // Apple-grade label type: Inter (SIL OFL, SF lookalike) replaces the
    // quirky styrene faces; sizes mirror the styrene tiers they replace.
    G.label_font = (G.tile >= 120) ? &font_inter_28
                 : (W    >= 400)   ? &font_inter_16
                                   : &font_inter_14;
}

// Gesture arbiter: the engine arms on horizontal intent (dx > 12px); a swipe
// release must NOT also fire the tile's CLICKED (LVGL suppresses clicks on
// scroll only for indev-driven scrollable objects — not our case). The flag
// is set when the engine arms and cleared when the touch fully resolves.
static bool swipe_suppress_click = false;

static void tile_clicked_cb(lv_event_t* e) {
    if (swipe_suppress_click) return;   // swipe release, not a tap
    shell_open((int)(intptr_t)lv_event_get_user_data(e));
}

// ---- Wallpaper -------------------------------------------------------------
// Apple's glass never sits on dead black — the translucent material needs
// something to bend. A vertical multi-stop "studio" gradient does it in one
// draw op: a faint cold daylight at the top, near-black at the corners, so
// AMOLED blacks and battery survive.
static void build_wallpaper(lv_obj_t* parent, int w, int h) {
    // Base: one wide VER duotone warm→cool. LVGL's radial gradients are off in
    // this build (LV_USE_DRAW_SW_COMPLEX_GRADIENTS unset), so the directional
    // flow is layered as sibling VER bands with per-stop alpha ramps — the
    // compositor alpha-grads at these deltas don't band and reads as light
    // pooling top-left, the way the Apple Waveform wallpaper does.
    struct { lv_color_t c; int stop; } stops[] = {
        { THEME_BG_TOP, 0 },      // warm taupe light
        { THEME_BG_MID, 118 },    // plum fall-off (ends ~52% down)
        { THEME_BG_FLOOR, 235 },  // gently-lifted navy floor (no hard hold)
        { THEME_BG_FLOOR, 255 },  // held to the bottom (battery)
    };
    lv_obj_t* wp = lv_obj_create(parent);
    lv_obj_set_size(wp, w, h);
    lv_obj_set_pos(wp, 0, 0);
    // Three stops — cool light pooling at the top, near-black at the base —
    // with the mid stop close to the top one so the falloff is smooth and the
    // short spans between stops can't band (RGB565 + no dithering).
    lv_obj_set_style_bg_color(wp, stops[0].c, 0);
    lv_obj_set_style_bg_grad_color(wp, stops[2].c, 0);
    lv_obj_set_style_bg_grad_dir(wp, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(wp, 0, 0);
    lv_obj_set_style_bg_grad_stop(wp, 230, 0);
    lv_obj_set_style_border_width(wp, 0, 0);
    lv_obj_set_style_pad_all(wp, 0, 0);
    lv_obj_set_style_bg_opa(wp, LV_OPA_COVER, 0);
    lv_obj_clear_flag(wp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(wp, LV_OBJ_FLAG_CLICKABLE);

    // Directional light bands: two warm VER alpha-ramp slabs, upper-left
    // biased. Stop opas peak ~50 (soft) — large-area, no hard edges, so no
    // banding; low alpha over the taupe reads as the wave crest catching sun.
    // Backdrop wash strata (r9 decider: compositional richness is the last
    // in-envelope lever): three broad REGIONAL washes with hue variation —
    // warm taupe crest (TL), plum mid-field basin (S), cool basin (E).
    // Feathered geography (r10: axis-aligned wash seams read as "drawn
    // rectangles"): every wash is a full-radius ellipse, oversized so its
    // rim leaves the panel — geography with no visible boundary lines.
    struct { int x, y, w, h; lv_color_t c; int opa; } washes[] = {
        { -w / 6, -h / 6, w, h,          SHEEN_TINT,              50 },  // warm crest TL
        { -w / 8, -h / 16, w, h * 3 / 4, lv_color_hex(0x3a2438),  30 },  // plum sweep
        { w - w / 4, h / 6, w + w / 3, h, lv_color_hex(0x14203a), 26 },  // cool basin E (runs off-panel)
    };
    for (unsigned i = 0; i < sizeof(washes) / sizeof(washes[0]); i++) {
        lv_obj_t* band = lv_obj_create(parent);
        lv_obj_set_size(band, washes[i].w, washes[i].h);
        lv_obj_set_pos(band, washes[i].x, washes[i].y);
        lv_obj_set_style_radius(band, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(band, washes[i].c, 0);
        lv_obj_set_style_bg_grad_color(band, washes[i].c, 0);
        // VER alpha ramp INSIDE the ellipse: strong core, feathered rim.
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
    struct { int x, w; int peak; int span; int ofs; } bands[] = {
        { 0,            (w * 3) / 4, 50, 200, 0  },  // main crest, top-left
        { (w * 5) / 10, w / 2,       36, 160, 60 },  // secondary, lower-right of crest
    };
    // Form continuation (critic r6: streaks must carry through the glass):
    // two thin elliptical arcs in warm cream, low opa, crossing the tile
    // rows — the tiles' translucent fills show them bending through the slab.
    // HERO form (r10: the sweep died mid-panel; a hairline ring reads as a
    // scratch): one giant cream LIGHT DOME whose top edge is off-panel and
    // whose lower rim arcs across the upper field — one luminous ridge, no
    // visible endpoints, glass tiles inherit its light from the fills.
    lv_obj_t* dome = lv_obj_create(parent);
    lv_obj_set_size(dome, w + w * 2 / 3, h);
    lv_obj_set_pos(dome, -w / 4, -h * 5 / 8);
    lv_obj_set_style_radius(dome, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dome, SHEEN_TINT, 0);
    lv_obj_set_style_bg_opa(dome, 34, 0);
    lv_obj_set_style_bg_grad_color(dome, SHEEN_TINT, 0);
    lv_obj_set_style_bg_grad_opa(dome, 0, 0);
    lv_obj_set_style_bg_grad_dir(dome, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(dome, 0, 0);
    lv_obj_set_style_bg_grad_stop(dome, 255, 0);
    lv_obj_set_style_border_color(dome, RIM_CREAM, 0);
    lv_obj_set_style_border_width(dome, 3, 0);
    lv_obj_set_style_border_opa(dome, 55, 0);
    lv_obj_set_style_border_side(dome, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_outline_width(dome, 0, 0);
    lv_obj_clear_flag(dome, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dome, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dome, LV_OBJ_FLAG_IGNORE_LAYOUT);
    for (unsigned i = 0; i < sizeof(bands) / sizeof(bands[0]); i++) {
        lv_obj_t* band = lv_obj_create(parent);
        lv_obj_set_size(band, bands[i].w, h);
        lv_obj_set_pos(band, bands[i].x, 0);
        lv_obj_set_style_bg_color(band, SHEEN_TINT, 0);
        lv_obj_set_style_bg_opa(band, bands[i].peak, 0);
        lv_obj_set_style_bg_grad_color(band, SHEEN_TINT, 0);
        lv_obj_set_style_bg_grad_opa(band, 0, 0);
        lv_obj_set_style_bg_grad_dir(band, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_stop(band, 0, 0);
        lv_obj_set_style_bg_grad_stop(band, bands[i].span, 0);
        lv_obj_set_style_border_width(band, 0, 0);
        lv_obj_set_style_pad_all(band, 0, 0);
        lv_obj_set_style_radius(band, 0, 0);
        lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
        (void)stops[1].stop; (void)stops[3].stop; (void)bands[i].ofs;
    }
}

// ---- Glass tile recipe -----------------------------------------------------
// One function owns the material so every tile reads as the same object.
// Layers, bottom to top:
//   1. soft drop shadow — a low puddle that grounds the glass on the wallpaper
//   2. body fill — ONE vertical gradient carries the whole material: the top
//      stop is the sheen (the hue washed with white — light caught in the
//      slab), the bottom stop is the pool (the hue dense where light gathers)
//   3. rim — bright border exactly ON the tile edge, radius-matched, plus a
//      thin dark outline OUTSIDE it that seats the tile on the wallpaper
//   4. glyph — plain bright glyph, optically centered
// The sheen lives in the fill itself (Apple anatomy), not in a child stripe —
// an alpha-graded white child measured at ~5 luma effective and compositor
// alpha-gradients banded at pill scale, so the fill does all the work.
// wall_base_at(): the wallpaper's own color behind a tile slot. The duotone
// runs taupe (top stop) -> navy (bottom stop 230/255); a tile's bottom pool
// must pull THIS light through the glass so the pool matches the backdrop
// (the critic's core criterion: tint pools match the wallpaper behind them).
static lv_color_t wall_base_at(int row) {
    return (row == 0) ? lv_color_mix(THEME_BG_BOT, THEME_BG_TOP, 120)
                      : lv_color_mix(THEME_BG_BOT, THEME_BG_TOP, 55);
}

// ---- iOS-class app icon ramps (the factory-desktop look) --------------------
// The reference home screen: opaque squircles, saturated two-stop gradients,
// white FPhosphor fill glyphs (dark ink on the silver tile only).
struct IconRamp { uint32_t rgb; uint32_t cap; uint32_t pool; bool dark_ink; };
static const IconRamp ICON_RAMPS[] = {
    { 0xb85739, 0xFF9F6E, 0xE25822, false },  // clawdmeter   — ember
    { 0x5f8f46, 0x7BD96C, 0x2F9E44, false },  // voice        — green
    { 0x7256b8, 0xB98EF2, 0x7B3FE4, false },  // sessions     — purple
    { 0x2b5b6e, 0x6FC3E8, 0x1673A9, false },  // clock        — blue
    { 0x8a4bc4, 0xD98EF5, 0x8B3DDE, false },  // SpecAnalyzer — violet
    { 0x3953c8, 0x7FA8FF, 0x2D5BE8, false },  // level        — royal blue
    { 0x7a4b6b, 0xE8A7C3, 0xC2558C, false },  // host         — plum-rose
    { 0xf2f2f7, 0xF7F7FA, 0xC9C9D1, true  },  // settings     — silver, dark ink
};
static const IconRamp* icon_ramp_for(uint32_t rgb) {
    for (auto& r : ICON_RAMPS) if (r.rgb == rgb) return &r;
    return &ICON_RAMPS[0];
}
static lv_color_t icon_glyph_color_of(const AppDef* d) {
    const IconRamp* r = icon_ramp_for(d->tile_rgb);
    return r->dark_ink ? lv_color_make(0x3a, 0x3a, 0x40)
                       : lv_color_make(0xFF, 0xFF, 0xFF);
}

// ---- iOS-class tile (the factory-desktop icon look) --------------------------
// Opaque squircle app icon: saturated two-stop ramp (bright cap → deep pool,
// same hue), the soft internal top-light, a hairline dark contour + a puddle
// shadow for seating on the photo. The glyph = a WHITE Phosphor-FILL icon
// (dark ink on the silver tile only) — the caller owns it. Pressed = the
// baked DIMMED variant (radius<0 sentinel so one recipe serves both bakes).
static void glass_tile(lv_obj_t* icon, const AppDef* d, int radius, int row) {
    const IconRamp* ramp = icon_ramp_for(d->tile_rgb);
    const lv_color_t cap  = lv_color_hex(ramp->cap);
    const lv_color_t pool = lv_color_hex(ramp->pool);
    const bool dim = (radius < 0);

    // Opaque body: bright cap → deep pool, chroma-dominant (the 16bpp
    // quantization rides the hue flow, not luma plateaus).
    lv_obj_set_style_bg_color(icon, cap, 0);
    lv_obj_set_style_bg_grad_color(icon, pool, 0);
    lv_obj_set_style_bg_opa(icon, dim ? LV_OPA_40 : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(icon, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(icon, 0, 0);
    lv_obj_set_style_bg_grad_stop(icon, 235, 0);

    // Seating: a soft puddle + a 1px darkened contour. No cream rim, no glass
    // glint children — the icon is self-lit, the app-icon class.
    lv_obj_set_style_shadow_color(icon, GLASS_SHADOW, 0);
    lv_obj_set_style_shadow_width(icon, 16, 0);
    lv_obj_set_style_shadow_spread(icon, 0, 0);
    lv_obj_set_style_shadow_ofs_y(icon, 6, 0);
    lv_obj_set_style_shadow_opa(icon, dim ? 45 : 70, 0);
    lv_obj_set_style_border_color(icon, lv_color_mix(pool, lv_color_hex(0x000000), 55), 0);
    lv_obj_set_style_border_width(icon, 1, 0);
    lv_obj_set_style_border_opa(icon, 110, 0);

    // Internal top-light: one clipped white rounded rect over the upper half
    // (the iOS icons' inherent upper luminance).
    const int w = lv_obj_get_width(icon), h = lv_obj_get_height(icon);
    lv_obj_t* light = lv_obj_create(icon);
    lv_obj_set_size(light, w - 16, h / 2);
    lv_obj_align(light, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(light, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(light, dim ? 18 : 42, 0);
    lv_obj_set_style_radius(light, (w - 16) / 2, 0);
    lv_obj_set_style_border_width(light, 0, 0);
    lv_obj_set_style_pad_all(light, 0, 0);
    lv_obj_set_style_clip_corner(light, true, 0);
    lv_obj_clear_flag(light, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(light, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(light, LV_OBJ_FLAG_IGNORE_LAYOUT);
}

// (The round-3 tile bake was retired: the iOS icons = opaque + cheap, so
// the bake's snapshot machinery no longer pays for itself — and its halo
// composited seams over the photo. The live render = a few blits/strip.)
struct TileBake { lv_draw_buf_t* rest; lv_draw_buf_t* down; };

static void tile_press_swap_cb(lv_event_t* e) {
    lv_obj_t* img = (lv_obj_t*)lv_event_get_target(e);
    TileBake* b = (TileBake*)lv_obj_get_user_data(img);
    if (!b) return;
    lv_image_set_src(img, (const lv_image_dsc_t*)
        (lv_event_get_code(e) == LV_EVENT_PRESSED ? b->down : b->rest));
}

// ---- Icon bake (round 2: the swipe needs blits) -----------------------------
// The live icons render ~3x the baked cost per swipe frame in the sim bench
// (3.97 vs 1.26 ms); at S3 scale that is the difference between butter and
// chop. These are OPAQUE squircles — so the bake needs no wallpaper/halo at
// all (that halo was the seam bug): snapshot the bare icon + glyph w/ the
// PRESSED styles pre-applied for the dim variant.
#define ICON_BX (lv_display_get_horizontal_resolution(NULL) + 10)   // parked
#define ICON_BY 10                                                  // right of screen
static lv_draw_buf_t* iconBake(lv_obj_t* parent, const AppDef* d, bool pressed) {
    lv_obj_t* icon = lv_obj_create(lv_layer_top());  // scratch on the top
    lv_obj_set_size(icon, G.tile, G.tile);           // layer: above the wall +
    lv_obj_set_pos(icon, 0, 0);                      // the pages, AT (0,0) —
                                                     // always renderable; the
                                                     // snapshot happens during
                                                     // init, pre-first-frame
    glass_tile(icon, d, G.radius, 0);
    lv_obj_set_style_radius(icon, G.radius, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(icon, pressed ? LV_OPA_40 : LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_opa(icon, 0, 0);   // no shadow in the bake: its
    lv_obj_set_style_border_opa(icon, 40, 0);  // ext ring = the black gutters
    lv_obj_set_style_shadow_width(icon, 0, 0);

    lv_obj_t* glyph = lv_label_create(icon);
    lv_label_set_text(glyph, d->glyph);
    lv_obj_set_style_text_font(glyph, G.glyph_font, 0);
    lv_obj_set_style_text_color(glyph, icon_glyph_color_of(d), 0);
    lv_obj_set_style_text_opa(glyph, pressed ? 160 : 255, 0);
    lv_obj_center(glyph);
    lv_obj_set_style_text_letter_space(glyph, 0, 0);

    lv_obj_update_layout(icon);
    // ARGB8888: the snapshot's outside-the-radius corners = TRANSPARENT
    // (the 565 snapshot = flattened them to black = the "black squares").
    lv_draw_buf_t* buf = lv_snapshot_take(icon, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_delete(icon);
    return buf;
}

static void make_tile(lv_obj_t* page, int slot, int app_index) {
    const AppDef* d = shell_app_at(app_index);
    if (!d) return;

    const int col = slot % G.cols;
    const int row = slot / G.cols;
    const int cell_x = G.inset + col * G.cell_w;
    const int cell_y = row * G.cell_h + (row ? G.row_gap : 0);
    const int icon_x = cell_x + (G.cell_w - G.tile) / 2;

    // The icon = a pre-baked OPAQUE bitmap (no halo, no backdrop, no glass:
    // the squircle = self-contained; the photo = behind it, untouched).
    // Sim bench: the live icons = 3.97 ms/frame vs the baked = 1.26 → at S3
    // scale the baked path = the butter story.
    static TileBake bakes[16];
    const int bi = app_index & 15;
    bakes[bi].rest = iconBake(page, d, false);
    bakes[bi].down = iconBake(page, d, true);
    if (bakes[bi].rest && bakes[bi].down) {
        lv_obj_t* icon = lv_image_create(page);
        lv_obj_set_pos(icon, icon_x, cell_y);   // buf = 145² = the icon exactly
        lv_image_set_src(icon, (const lv_image_dsc_t*)bakes[bi].rest);
        lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);   // lv_image = an lv_obj:
                                                           // its default dark bg
                                                           // painted the boxes
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(icon, tile_clicked_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)app_index);
        lv_obj_add_event_cb(icon, tile_press_swap_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(icon, tile_press_swap_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(icon, tile_press_swap_cb, LV_EVENT_PRESS_LOST, NULL);
        lv_obj_set_user_data(icon, &bakes[bi]);
    } else {
        // Snapshot unavailable: the live icon (the same recipe + the dim).
        if (bakes[bi].rest)  lv_draw_buf_destroy(bakes[bi].rest);
        if (bakes[bi].down)  lv_draw_buf_destroy(bakes[bi].down);
        lv_obj_t* icon = lv_obj_create(page);
        lv_obj_set_size(icon, G.tile, G.tile);
        lv_obj_set_pos(icon, icon_x, cell_y);
        glass_tile(icon, d, G.radius, row);
        lv_obj_set_style_radius(icon, G.radius, 0);
        lv_obj_set_style_pad_all(icon, 0, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(icon, tile_clicked_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)app_index);
        lv_obj_set_style_bg_opa(icon, LV_OPA_40, LV_STATE_PRESSED);

        lv_obj_t* glyph = lv_label_create(icon);
        lv_label_set_text(glyph, d->glyph);
        lv_obj_set_style_text_font(glyph, G.glyph_font, 0);
        lv_obj_set_style_text_color(glyph, icon_glyph_color_of(d), 0);
        lv_obj_center(glyph);
    }

    // iOS label anatomy: white text + a 2px dark ghost (read over anything).
    lv_obj_t* ghost = lv_label_create(page);
    lv_label_set_text(ghost, d->title);
    lv_obj_set_style_text_font(ghost, G.label_font, 0);
    lv_obj_set_style_text_color(ghost, lv_color_hex(0x1a2612), 0);
    lv_obj_set_style_text_opa(ghost, 150, 0);
    lv_obj_set_width(ghost, G.cell_w);
    lv_obj_set_style_text_align(ghost, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ghost, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(ghost, cell_x, cell_y + G.tile + G.label_gap + 2);

    lv_obj_t* label = lv_label_create(page);
    lv_label_set_text(label, d->title);
    lv_obj_set_style_text_font(label, G.label_font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_width(label, G.cell_w);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(label, cell_x, cell_y + G.tile + G.label_gap);
}

int swipe_page_active(void);        // engine forward decl (defined below)

static void refresh_dots(void) {
    if (!dots_row) return;
    const int active = (page_count > 1)
        ? (int)swipe_page_active()
        : 0;
    for (int i = 0; i < page_count; i++) {
        lv_obj_t* dot = lv_obj_get_child(dots_row, i);
        if (!dot) continue;
        lv_obj_set_style_bg_opa(dot, i == active ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

// ---- swipe engine state ----------------------------------------------------
// One frame paints engine band rows from baked page bitmaps straight to GRAM:
// the moving edge = each frame's tile rows, the stable remainder sits behind.
// GRAM auto-increments the write pointer, so painting successive row bands
// left-to-right costs only the actual slide distance per frame.
// The swipe engine = ON TOP of the live widget stack (transparent lv_image
// OVERLAY on the top layer during a swipe; the live tiles hide, not render).
enum SwipePhase { SWIPE_IDLE, SWIPE_TOUCH, SWIPE_SETTLE };
static SwipePhase swipe_state = SWIPE_IDLE;
static float   swipe_offset = 0;        // page-space scroll (0 = page 0 home)
static float   swipe_frac = 0;          // settle frac 0..1
static uint32_t swipe_start_ms = 0;
static int16_t swipe_touch_x = 0;
static int     page_visible = 0;        // settled page
static bool    swipe_move_seen = false;   // a real move() arrived this touch
static bool    latch_seen = false;        // arm-on-intent: latch currently down
static int16_t latch_x = 0, latch_y = 0;
static uint16_t* band_buf = nullptr;   // W×STRIP scratch (PSRAM)
static float painted_offset = 0;        // GRAM truth: last painted offset
static lv_draw_buf_t* page_bakes[8] = {};

static uint32_t last_move_ms = 0;    // finger velocity tracking (flick test)
static int16_t  prev_move_x = 0;
static float    flick_v = 0;         // smoothed px/ms

static bool bench_drives = false;   // bench script owns the touch stream (tick skips the latch)

static float engine_curve(float t) {
    return t < 0 ? 0 : (t > 1 ? 1 : (1 - t) * (1 - t) * (3 - 2 * t));
}
#define TAP_MS 220


void launcher_swipe_tick(void);
static void swipe_settle(float target_off, bool immediate);
static float swipe_choose_target(float pos);
static struct timespec bench_t0;
static void engine_render(void);
extern "C" void lv_display_render_inhibit_set(uint8_t v);
#include "../../src/hal/touch_hal.h"

// ---- Swipe bench (serial 'bench') ------------------------------------------
// Drives a scripted horizontal swipe and prints the wall-clock per frame, so
// render+flush cost measures directly — sim AND hardware.
#include <math.h>
#include <string.h>
static int bench_frame = 0;
static float bench_first = 0, bench_sum = 0;   // frame 0 (warmup/tap) excluded
void launcher_bench_start(void);
void launcher_bench_tick(void);
void launcher_bench_start(void) {
    if (page_count <= 1 || !root) return;
    bench_frame = 0;
    bench_first = 0; bench_sum = 0;
    bench_drives = true;
    Serial.println("[bench] engine swipe bench: start");
    // one scripted swipe: press x=100, sweep to x=100+W over ~14 bench frames,
    // release; settle anim measured separately (swipe_state==SETTLE frames).
    lv_obj_scroll_to_x(pages, 0, LV_ANIM_OFF);
    page_visible = 0;
    bench_frame = -1;                       // -1 = next tick arms the press
}
void launcher_bench_tick(void) {
    extern volatile uint32_t bench_flush_px;
    extern volatile uint32_t bench_flush_calls;
    extern volatile uint32_t bench_flush_last_w, bench_flush_last_h;
    if (bench_frame == -2) return;                       // sweep done
    if (bench_frame == -1) {                             // arm the scripted press
        bench_frame = 0;
        launcher_swipe_press(100, 240);
        Serial.println("[bench] press armed");
        // consume the FIRST latch-up in arm_tick (it is the stale-read, not a lift)
        uint16_t x, y; bool p; touch_hal_read(&x, &y, &p);
        (void)x; (void)y;
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &bench_t0);
    if (swipe_state == SWIPE_TOUCH && bench_frame < 14) { // tracked move frames
        const int x = 100 + (bench_frame + 1) * (board_caps().width + 60) / 14;
        launcher_swipe_move(x, 240);
        lv_display_render_inhibit_set(1);   // bench-driven frame
        engine_render();
        lv_tick_inc(20);
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        float dt = (t1.tv_sec - bench_t0.tv_sec) * 1000.0f
                 + (t1.tv_nsec - bench_t0.tv_nsec) / 1e6f;
        if (bench_first == 0) bench_first = dt;
        else bench_sum += dt;
        Serial.printf("[bench] f%02d %6.2f ms  flush_px=%lu calls=%lu last=%lux%lu\n",
                      bench_frame++, dt, (unsigned long)bench_flush_px,
                      (unsigned long)bench_flush_calls,
                      (unsigned long)bench_flush_last_w, (unsigned long)bench_flush_last_h);
        return;
    }
    if (bench_frame <= 14) {                             // release → settle anim
        launcher_swipe_release();
        bench_frame = 15;
    }
    lv_tick_inc(20);
    launcher_swipe_tick();
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    float dt = (t1.tv_sec - bench_t0.tv_sec) * 1000.0f
             + (t1.tv_nsec - bench_t0.tv_nsec) / 1e6f;
    Serial.printf("[bench] s%02d %6.2f ms\n", bench_frame - 15, dt);
    if (swipe_state == SWIPE_IDLE && bench_frame > 15) {
        Serial.printf("[bench] done: avg %.2f ms/frame | settled page=%d offset=%.0f state=%d scroll_x=%ld\n",
                      bench_sum / 13, page_visible, (double)swipe_offset,
                      (int)swipe_state,
                      (long)lv_obj_get_scroll_x(pages));
        bench_frame = -2;
        bench_drives = false;
    } else bench_frame++;
}

// ============================================================================
// PAGE BAKE + BYPASS SWIPE ENGINE (the factory-firmware approach, measured)
// ============================================================================
// The live-widget page swipe re-renders ~195kpx per frame (2.16 measured:
// 100+ ms/frame = 10 FPS — the chop). Factory UIs slide prepared bitmaps. So:
// bake each page's tile area ONCE into PSRAM, keep the transparent LVGL page
// scroll for layout/tap truth, and from swipe-down paint each frame's GRAM
// band directly from the page bitmaps. LVGL renders no tiles mid-swipe; when
// the settle finishes, the pages scroll snaps and control hands back.
// RAM: pages × W × grid_h × 2 B in PSRAM (2 pages on the 2.16 = 1.5 MB).
// Sim note: engine paints via display_hal (SDL frame driver), identical path.
#include "draw/snapshot/lv_snapshot.h"
#include "../../src/hal/display_hal.h"
#include "../../src/hal/touch_hal.h"

#define STRIP 40   // engine render strip (matches main.cpp BUF_LINES)

// ---- page bake: snapshot the transparent page container ----
// The container carries no bg/border/pad; the tiles inside are opaque
// squircles with baked-outside shadow edges (BLANK_TILE + solid bg). Corners
// show BLANK_TILE, not the wallpaper (the live wallpaper keeps showing
// between the bakes as usual — the bake itself is opaque ONLY where tiles
// are: the snapshot records exactly what the container paints, plus shadow
// fringe pixels which read fine over the wall).
static lv_draw_buf_t* bake_page(int p) {
    const int W = board_caps().width;
    lv_obj_t* page = lv_obj_get_child(pages, p);
    if (!page) return nullptr;
    lv_obj_update_layout(page);
    return lv_snapshot_take(page, LV_COLOR_FORMAT_RGB565);
}

static void engine_render(void) {
    const int W = board_caps().width;
    const int H = board_caps().height;
    if (!band_buf) return;
    // Moved-band strategy: paint ONLY the panel columns the seam crossed this
    // frame (painted_offset → swipe_offset). Frame cost ∝ |dx| (a tracked
    #ifdef BOARD_HAS_PSRAM
    int old_x = (int)floorf(-painted_offset / (float)W);
    int new_x = (int)floorf(-swipe_offset / (float)W);
    int x_lo = (old_x < new_x) ? old_x : new_x;
    int x_hi = (old_x < new_x) ? new_x : old_x;
    if (x_hi <= x_lo) x_hi = x_lo + 1;               // ≥1px always moves
    if (x_lo < 0) x_lo = 0;
    if (x_hi > W) x_hi = W;
    if (x_hi <= x_lo) { painted_offset = swipe_offset; return; }
    const int bw = x_hi - x_lo;
    // 1) tile rows of the band: composite from the LVGL-space bakes
    const int gh = G.grid_h;
    const int sA = (int)floorf(-swipe_offset / (float)W);
    const float offmod = -swipe_offset - (float)sA * (float)W;
    const int frac = (int)(offmod + 0.5f);
    const lv_draw_buf_t* A = (sA >= 0 && sA < page_count) ? page_bakes[sA] : nullptr;
    const lv_draw_buf_t* B = (sA + 1 >= 0 && sA + 1 < page_count) ? page_bakes[sA + 1] : nullptr;
    const uint16_t* a = A ? (const uint16_t*)A->data : nullptr;
    const uint16_t* b = B ? (const uint16_t*)B->data : nullptr;
    // full-height composite band in LVGL space, then row-strip-rotate-push:
    // process in 40-row strips so band_buf stays small; rotate_strip maps a
    // sub-rect (sx,sy,w,h) of the LVGL frame into panel coords.
    for (int sy = 0; sy < H; sy += STRIP) {
        const int rows = (H - sy < (int)STRIP) ? H - sy : (int)STRIP;
        for (int r = 0; r < rows; r++) {
            uint16_t* dst = band_buf + (size_t)r * bw;
            const int lvgl_y = sy + r;
            const bool in_tiles = (lvgl_y >= STATUSBAR_H && lvgl_y < STATUSBAR_H + gh);
            const int bake_y = lvgl_y - STATUSBAR_H;
            for (int x = 0; x < bw; x++) {
                int px = 0;
                uint16_t c = lv_color_to_u16(THEME_BG);
                if (in_tiles) {
                    const int lvgl_x = x_lo + x;
                    px = lvgl_x + sA * W + frac;    // bake A column of this px
                    int bpx = px - W;
                    if (a && px >= 0 && px < W)
                        c = a[(size_t)bake_y * W + px];
                    else if (b && bpx >= 0 && bpx < W)
                        c = b[(size_t)bake_y * W + bpx];
                    else
                        c = lv_color_to_u16(THEME_BG);  // beyond both bakes
                }
                dst[x] = c;
            }
        }
        // 2) push the band through the display: the frame sub-rect (x_lo, sy,
        //    bw, rows) of LVGL space — rotate_strip maps it to panel coords.
        display_hal_draw_bitmap(x_lo, sy, bw, rows, band_buf);
    }
    painted_offset = swipe_offset;
    #endif
}
// frame strip rows which CROSS chrome boundaries (statusbar/tile area)
// render-suppression hooks the engine flips for its own frames.
// C linkage: the lvgl library is C — the inhibit hook is read from C code
// (lv_refr.c / lv_draw_sw.c RENDER_INHIBIT patches), so it must match.
static bool engine_inhibit = false;
extern "C" uint8_t lv_display_render_inhibit_get(void) { return engine_inhibit ? 1 : 0; }
extern "C" void lv_display_render_inhibit_set(uint8_t v) { engine_inhibit = v != 0; }

// --- lv_refr.c / lv_draw_sw.c render-inhibit enforcement (v9.2 libdeps) ---
// (auto-patched by a session script; see .session-state.md; re-run
// tools/verify_refr_patch.sh after a libdeps re-fetch.)


// bake all pages into page_bakes (8 max)
static void bake_all_pages(void) {
    for (int p = 0; p < page_count && p < 8; p++) page_bakes[p] = bake_page(p);
    int ok = 0;
    for (int p = 0; p < page_count && p < 8; p++) if (page_bakes[p]) ok++;
    Serial.printf("[engine] baked %d/%d pages\n", ok, page_count);
}

// ---- engine tick: TOUCH tracking + SETTLE anim ------------------------------
void launcher_swipe_press(int x, int y) {
    (void)y;
    if (page_count <= 1 || !root) return;
    // arm the engine: inhibit LVGL rendering while the band owns the paint
    lv_display_render_inhibit_set(1);
    swipe_state = SWIPE_TOUCH;
    swipe_touch_x = x;
    prev_move_x = x;
    last_move_ms = lv_tick_get();
    flick_v = 0;
    swipe_suppress_click = true;    // this drag must not click a tile
    swipe_start_ms = lv_tick_get();
    swipe_frac = 0;
    swipe_move_seen = false;
    painted_offset = -(float)page_visible * (float)board_caps().width;
}

void launcher_swipe_move(int x, int y) {
    (void)y;
    if (swipe_state != SWIPE_TOUCH) return;
    swipe_move_seen = true;
    const int dx = x - swipe_touch_x;
    const int W = board_caps().width;
    // finger velocity (px/ms), smoothed — feeds the flick decision on release
    const uint32_t now = lv_tick_get();
    if (now != last_move_ms) {
        const float inst = (float)(x - prev_move_x) / (float)(now - last_move_ms);
        flick_v = 0.6f * flick_v + 0.4f * inst;
        last_move_ms = now;
        prev_move_x = x;
    }
    // follow the finger with overscroll resistance at the ends
    float v = -page_visible * (float)W + dx;
    const float lo = 0, hi = (float)(page_count - 1) * W;
    if (v < lo) v = lo + (v - lo) * 0.35f;
    if (v > hi) v = hi + (v - hi) * 0.35f;
    swipe_offset = -v;
}

void launcher_swipe_release(void) {
    if (swipe_state != SWIPE_TOUCH) return;
    const int W = board_caps().width;
    const float pos = -swipe_offset / W;         // 0..page_count-1 position
    const bool moved_far = fabsf(pos - page_visible) > 0.32f;
    // Tap (brief + small move) = no page change (tile tap handling proceeds)
    if (lv_tick_get() - swipe_start_ms < TAP_MS && !moved_far) {
        // brief + small = a tap, not a swipe: click stays allowed
        swipe_suppress_click = false;
        swipe_state = SWIPE_IDLE;
        return;
    }
    swipe_settle(swipe_choose_target(pos), false);
}

// iOS release rule, shared by the release path and the tick's settle:
// a fast short flick advances one page in the flick direction; anything
// slower snaps to the nearest page by position.
static float swipe_choose_target(float pos) {
    const int W = board_caps().width;
    if (fabsf(flick_v) > 0.55f) {
        int target = page_visible + (flick_v < 0 ? 1 : -1);  // left flick = next
        if (target < 0)            target = 0;
        if (target > page_count-1) target = page_count - 1;
        if (target != page_visible) return -(float)target * (float)W;
    }
    return roundf(pos) * -(float)W;                 // page k lives at -k*W
}

static float settle_from   = 0;   // offset when settle started
static float settle_target = 0;

static void swipe_settle(float target_off, bool immediate) {
    settle_target = target_off;
    settle_from   = swipe_offset;
    if (immediate) swipe_offset = target_off;
    swipe_state  = SWIPE_SETTLE;
    swipe_start_ms = lv_tick_get();
    swipe_frac   = immediate ? 1 : 0;
}

void launcher_swipe_tick(void) {
    const int W = board_caps().width;
    switch (swipe_state) {
    case SWIPE_TOUCH: {
        if (!bench_drives) {
            uint16_t x, y; bool p;
            touch_hal_read(&x, &y, &p);   // same latch LVGL reads: no steal
                if (p) { lv_display_render_inhibit_set(1); launcher_swipe_move(x, y); engine_render(); return; }
            // Latch reads UP: settle from wherever we got to (move_seen
            // guards a stale latch: press with no fresh sample anywhere).
            if (!swipe_move_seen) {
                swipe_state = SWIPE_IDLE;
                swipe_suppress_click = false;
                break;
            }
            const float pos = -swipe_offset / (float)W;
            swipe_settle(swipe_choose_target(pos), false);
        }
        break;
    }
    case SWIPE_SETTLE: {
        const uint32_t el = lv_tick_get() - swipe_start_ms;
        swipe_frac = engine_curve(el / 190.0f);      // ~190ms settle (iOS feel)
        swipe_offset = settle_from
                     + (settle_target - settle_from) * swipe_frac;
        lv_display_render_inhibit_set(1);
        engine_render();
        if (el >= 190) {
            // hand back to LVGL: the last engine frame IS the settled state
            // (no full invalidate: the exact pixels are already on glass)
            page_visible = (int)roundf(-swipe_offset / (float)W);
            swipe_state = SWIPE_IDLE;
            lv_obj_scroll_to_x(pages, page_visible * (float)W, LV_ANIM_OFF);
            painted_offset = swipe_offset;
            lv_display_render_inhibit_set(0);
            swipe_suppress_click = false;   // gesture fully resolved
            refresh_dots();
        }
        break;
    }
    case SWIPE_IDLE: {
        // arm-on-intent: the latch is down and a horizontal move exceeds the
        // tap threshold → the engine takes over (bypass swipe). Vertical /
        #ifdef BOARD_HAS_PSRAM
        uint16_t x, y; bool p;
        touch_hal_read(&x, &y, &p);
        if (p) {
            if (!latch_seen) { latch_seen = true; latch_x = x; latch_y = y; }
            const int dx = (int)x - latch_x, dy = (int)y - latch_y;
            if ((dx > 12 || dx < -12)) {           // HORIZONTAL intent
                launcher_swipe_press(latch_x, latch_y);
                launcher_swipe_move(x, y);
                engine_render();
                return;
            }
        } else latch_seen = false;
        #endif
        break;
    }
    default: break;
    }
}

int swipe_page_active(void) { return page_visible; }

void launcher_init(lv_obj_t* parent) {
    compute_layout(board_caps());
    const int W = board_caps().width;
    const int H = board_caps().height;

    root = lv_obj_create(parent);
    lv_obj_set_size(root, W, H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    // Wallpaper: the Waveshare Lake-Quinault reference photo (the factory
    // look the user asked for), baked as a 480x480 RGB565 asset — already
    // ONE opaque image, so the round-1 snapshot flatten = unnecessary here;
    // the duotone stack remains the fallback (sim/no-asset path).
    // Tile bakes composite the wallpaper behind each slot, so the glass
    // picks up the photo's light per-slot with no recipe changes.
    {
        extern const lv_image_dsc_t img_wall_photo;
        lv_obj_t* wall_img = lv_image_create(root);
        lv_image_set_src(wall_img, &img_wall_photo);
        lv_obj_set_pos(wall_img, 0, 0);
        lv_obj_clear_flag(wall_img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(wall_img, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    const int n = shell_app_count();
    page_count = (n + G.per_page - 1) / G.per_page;
    if (page_count < 1) page_count = 1;

    // One horizontally-snapping row of full-width pages gives iOS-style
    // paging with momentum for free. With a single page we turn scrolling off
    // entirely so a stray horizontal drag can't drift the grid.
    pages = lv_obj_create(root);
    lv_obj_set_size(pages, W, G.grid_h);
    lv_obj_set_pos(pages, 0, STATUSBAR_H);
    lv_obj_set_style_bg_opa(pages, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pages, 0, 0);
    lv_obj_set_style_pad_all(pages, 0, 0);
    lv_obj_set_scrollbar_mode(pages, LV_SCROLLBAR_MODE_OFF);

    // The bypass swipe engine is the only scroll authority: LVGL indev
    // scrolling stays OFF (it fought the engine mid-gesture and its scroll
    // throw kept dragging after handback — the "hard to swipe" feel).
    lv_obj_clear_flag(pages, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pages, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(pages, LV_DIR_HOR);

    for (int p = 0; p < page_count; p++) {
        lv_obj_t* page = lv_obj_create(pages);
        lv_obj_set_size(page, W, G.grid_h);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        if (page_count == 1) lv_obj_set_pos(page, 0, 0);

        for (int slot = 0; slot < G.per_page; slot++) {
            const int idx = p * G.per_page + slot;
            if (idx >= n) break;
            make_tile(page, slot, idx);
        }
    }

    if (page_count > 1) {
        dots_row = lv_obj_create(root);
        lv_obj_set_size(dots_row, W, 14);
        lv_obj_set_pos(dots_row, 0, STATUSBAR_H + G.grid_h + 2);
        lv_obj_set_style_bg_opa(dots_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(dots_row, 0, 0);
        lv_obj_set_style_pad_all(dots_row, 0, 0);
        lv_obj_clear_flag(dots_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(dots_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(dots_row, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(dots_row, 10, 0);

        for (int i = 0; i < page_count; i++) {
            lv_obj_t* dot = lv_obj_create(dots_row);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, THEME_TEXT, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
            // Active page: iOS-style wide glass pill instead of a bigger dot.
            if (i == 0) {
                lv_obj_set_size(dot, 15, 7);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_size(dot, 7, 7);
                lv_obj_set_style_bg_opa(dot, 40, 0);
            }
        }
        refresh_dots();
    }

    bake_all_pages();   // tile-area snapshots for the swipe engine

    band_buf = (uint16_t*)heap_caps_malloc((size_t)board_caps().width
                                           * STRIP * 2, MALLOC_CAP_SPIRAM);
    if (!band_buf) Serial.println("[engine] band_buf alloc failed");

#ifdef BOARD_SIM
    Serial.println("[bench] sim: 'bench' cmd = scripted engine bench");
#endif
}

void launcher_goto_page(int page) {
    if (!pages || page_count <= 1) return;
    if (page < 0)           page = 0;
    if (page >= page_count) page = page_count - 1;
    swipe_settle(-(float)page * board_caps().width, true);
    page_visible = page;
    refresh_dots();
}

int launcher_page_count(void) { return page_count; }

void launcher_set_visible(bool visible) {
    if (!root) return;
    if (visible) lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}
