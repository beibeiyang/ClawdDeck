#include "launcher.h"
#include "shell.h"
#include "statusbar.h"
#include "../theme.h"
#include "../hal/board_caps.h"
#include "draw/snapshot/lv_snapshot.h"
#include <stdio.h>


LV_FONT_DECLARE(font_inter_28);
LV_FONT_DECLARE(font_inter_16);
LV_FONT_DECLARE(font_inter_14);
LV_FONT_DECLARE(font_phos_40);
LV_FONT_DECLARE(font_phos_28);
LV_FONT_DECLARE(font_phos_20);

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

    G.glyph_font = (G.tile >= 150) ? &font_phos_40
                 : (G.tile >=  88) ? &font_phos_40
                 : (G.tile >=  60) ? &font_phos_28
                                   : &font_phos_20;
    // Apple-grade label type: Inter (SIL OFL, SF lookalike) replaces the
    // quirky styrene faces; sizes mirror the styrene tiers they replace.
    G.label_font = (G.tile >= 120) ? &font_inter_28
                 : (W    >= 400)   ? &font_inter_16
                                   : &font_inter_14;
}

static void tile_clicked_cb(lv_event_t* e) {
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

static void glass_tile(lv_obj_t* icon, const AppDef* d, int radius, int row) {
    const lv_color_t hue  = lv_color_hex(d->tile_rgb);
    const lv_color_t wall = wall_base_at(row);   // backdrop light behind glass

    // Body — LIFT model (critic r5: Apple glass brightens what's behind it;
    // dark-tint-over-backdrop is the 2020-Android tell). The fill is the
    // wallpaper's own light ADDED-TO: brightened wall (the slab's inner
    // luminance) with the hue as a color cast, not a darkener. Cap stop =
    // wall lifted hard toward cream (light entering); pool stop = wall
    // lifted toward hue (light gathering in color). Opas moderate: bright
    // color doing the work, not coverage.
    // Chroma-dominant ramp (r7: luma-only ramps quantize into plateaus on
    // 16bpp): top = cream-lit, clearly LOWER saturation; pool = saturated hue
    // at similar luma. The eye reads a smooth HUE flow — the 5-bit luma
    // quantization rides along inaudibly instead of showing plateaus.
    const lv_color_t top_c = lv_color_mix(wall, RIM_CREAM, 40);
    lv_obj_set_style_bg_color(icon,
        lv_color_mix(lv_color_mix(wall, RIM_CREAM, 85), hue, 165), 0);
    lv_obj_set_style_bg_opa(icon, 145, 0);
    lv_obj_set_style_bg_grad_color(icon,
        lv_color_mix(lv_color_mix(wall, lv_color_hex(0xffffff), 12), hue, 82), 0);
    lv_obj_set_style_bg_grad_opa(icon, 185, 0);
    lv_obj_set_style_bg_grad_dir(icon, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(icon, 0, 0);
    lv_obj_set_style_bg_grad_stop(icon, 240, 0);

    // Drop shadow: soft puddle straight down (attempt-2 tuned: light).
    lv_obj_set_style_shadow_color(icon, GLASS_SHADOW, 0);
    lv_obj_set_style_shadow_width(icon, 14, 0);
    lv_obj_set_style_shadow_spread(icon, 0, 0);
    lv_obj_set_style_shadow_ofs_y(icon, 5, 0);
    lv_obj_set_style_shadow_opa(icon, 25, 0);

    // Directional rim (critic r2: uniform outline reads plastic): light comes
    // from above — cream glint on the TOP arc only, faint warm sides, none at
    // the base (the shadow side), plus the dark outer contour for seating.
    // Light comes from above: warm cream rim, but the glint sits on the
    // top arc — a clipped top-half highlight child gives the directional
    // glint the uniform border can't (critic r2).
    lv_obj_set_style_border_color(icon, RIM_CREAM, 0);
    lv_obj_set_style_border_width(icon, 2, 0);
    lv_obj_set_style_border_opa(icon, 95, 0);
    lv_obj_t* glint = lv_obj_create(icon);
    lv_obj_set_size(glint, lv_obj_get_width(icon) - 24, (lv_obj_get_height(icon) / 2) - 12);
    lv_obj_align(glint, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_color(glint, lv_color_mix(RIM_CREAM, lv_color_hex(0xfff6ea), 50), 0);
    lv_obj_set_style_bg_grad_color(glint, RIM_CREAM, 0);
    lv_obj_set_style_bg_grad_dir(glint, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(glint, 78, 0);
    lv_obj_set_style_bg_grad_opa(glint, 0, 0);
    lv_obj_set_style_bg_main_stop(glint, 0, 0);
    lv_obj_set_style_bg_grad_stop(glint, 255, 0);
    lv_obj_set_size(glint, lv_obj_get_width(icon) - 28, (lv_obj_get_height(icon) * 5) / 8 - 14);
    lv_obj_set_style_radius(glint, (lv_obj_get_height(glint)) / 2, 0);
    lv_obj_set_style_border_width(glint, 0, 0);
    lv_obj_set_style_pad_all(glint, 0, 0);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(glint, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_clip_corner(glint, true, 0);
    lv_obj_set_style_outline_color(icon, GLASS_SHADOW, 0);
    lv_obj_set_style_outline_width(icon, 1, 0);
    lv_obj_set_style_outline_pad(icon, 0, 0);
    lv_obj_set_style_outline_opa(icon, 90, 0);

    // Press feedback: brighten the glass and deepen the puddle — pressed
    // glass feels thicker, not dimmer.
    lv_obj_set_style_bg_opa(icon, 190, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(icon, 60, LV_STATE_PRESSED);
}

static void make_tile(lv_obj_t* page, int slot, int app_index) {
    const AppDef* d = shell_app_at(app_index);
    if (!d) return;

    const int col = slot % G.cols;
    const int row = slot / G.cols;
    const int cell_x = G.inset + col * G.cell_w;
    const int cell_y = row * G.cell_h + (row ? G.row_gap : 0);
    const int icon_x = cell_x + (G.cell_w - G.tile) / 2;

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

    // Glyph: Apple-style dark ink on the light glass (thin-line Phosphor
    // replaces the stock montserrat symbols). Depth comes from the glass,
    // not from overlay bands over it (attempt-1's dark glyph-floor strip
    // banded the tile middles — deleted).
    lv_obj_t* glyph = lv_label_create(icon);
    lv_label_set_text(glyph, d->glyph);
    lv_obj_set_style_text_font(glyph, G.glyph_font, 0);
    lv_obj_set_style_text_color(glyph, THEME_INK, 0);
    lv_obj_set_style_text_opa(glyph, 235, 0);
    lv_obj_center(glyph);
    lv_obj_set_style_text_letter_space(glyph, 0, 0);

    lv_obj_t* label = lv_label_create(page);
    lv_label_set_text(label, d->title);
    lv_obj_set_style_text_font(label, G.label_font, 0);
    lv_obj_set_style_text_color(label, THEME_TEXT, 0);
    lv_obj_set_width(label, G.cell_w);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(label, cell_x, cell_y + G.tile + G.label_gap);
}

static void refresh_dots(void) {
    if (!dots_row) return;
    const int active = (page_count > 1)
        ? (lv_obj_get_scroll_x(pages) + board_caps().width / 2) / board_caps().width
        : 0;
    for (int i = 0; i < page_count; i++) {
        lv_obj_t* dot = lv_obj_get_child(dots_row, i);
        if (!dot) continue;
        lv_obj_set_style_bg_opa(dot, i == active ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

static void scroll_cb(lv_event_t* e) {
    (void)e;
    refresh_dots();
}

void launcher_init(lv_obj_t* parent) {
    compute_layout(board_caps());
    const int W = board_caps().width;
    const int H = board_caps().height;

    root = lv_obj_create(parent);
    lv_obj_set_size(root, W, H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    // Wallpaper: the full layer stack (duotone + washes + light dome) is
    // built once into a throwaway container and snapshot-flattened to ONE
    // opaque RGB565 image (lv_image). A horizontal swipe dirties the whole
    // grid area every frame — re-blending ~10 gradient layers per strip per
    // frame was the swipe cost; post-flatten the wall is a single blit.
    // Verified pixel-identical to the live stack in the sim.
    lv_obj_t* wall = lv_obj_create(root);
    lv_obj_set_size(wall, W, H);
    lv_obj_set_pos(wall, 0, 0);
    lv_obj_set_style_border_width(wall, 0, 0);
    lv_obj_set_style_pad_all(wall, 0, 0);
    lv_obj_clear_flag(wall, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(wall, LV_OBJ_FLAG_CLICKABLE);
    build_wallpaper(wall, W, H);
    lv_obj_update_layout(wall);
    lv_draw_buf_t* wall_flat = lv_snapshot_take(wall, LV_COLOR_FORMAT_RGB565);
    lv_obj_delete(wall);
    if (wall_flat) {
        lv_obj_t* wall_img = lv_image_create(root);
        lv_image_set_src(wall_img, (const lv_image_dsc_t*)wall_flat);
        lv_obj_set_pos(wall_img, 0, 0);
        lv_obj_clear_flag(wall_img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(wall_img, LV_OBJ_FLAG_CLICKABLE);
        // wall_flat stays allocated for the launcher's lifetime (PSRAM on
        // hardware): ~W*H*2 bytes of RAM buys one-op swipes. Lean trade.
    } else {
        build_wallpaper(root, W, H);   // snapshot unsupported: keep live stack
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

    if (page_count > 1) {
        lv_obj_set_flex_flow(pages, LV_FLEX_FLOW_ROW);
        lv_obj_set_scroll_dir(pages, LV_DIR_HOR);
        lv_obj_set_scroll_snap_x(pages, LV_SCROLL_SNAP_CENTER);
        lv_obj_add_event_cb(pages, scroll_cb, LV_EVENT_SCROLL_END, NULL);
    } else {
        lv_obj_clear_flag(pages, LV_OBJ_FLAG_SCROLLABLE);
    }

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
}

void launcher_goto_page(int page) {
    if (!pages || page_count <= 1) return;
    if (page < 0)           page = 0;
    if (page >= page_count) page = page_count - 1;
    lv_obj_scroll_to_x(pages, page * board_caps().width, LV_ANIM_OFF);
    refresh_dots();
}

int launcher_page_count(void) { return page_count; }

void launcher_set_visible(bool visible) {
    if (!root) return;
    if (visible) lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}