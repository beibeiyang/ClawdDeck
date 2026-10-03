#include "../shell/app.h"
#include "phosphor_cp.h"
#include "../shell/shell.h"
#include "../theme.h"
#include "../hal/board_caps.h"
#include "../shell/statusbar.h"

LV_FONT_DECLARE(font_inter_28);
LV_FONT_DECLARE(font_inter_16);
LV_FONT_DECLARE(font_inter_14);
LV_FONT_DECLARE(font_phos_28);
LV_FONT_DECLARE(font_phos_40);

// Placeholder screens for apps that are designed but not built yet.
//
// Every AppDef below shares one create() because the screen renders itself
// from shell_foreground() — the shell publishes the AppDef it's constructing,
// so there's no need for eight near-identical stub functions. As each app is
// implemented it moves to apps/<id>/app.cpp with its own AppDef and drops out
// of this file; the registry entry in shell/apps.cpp is the only thing that
// changes. Tile colour, glyph, blurb and capability requirements are already
// final here, so the launcher looks and filters correctly today.
//
// The screen itself is cut from the same glass as the launcher tiles
// (shell/launcher.cpp glass_tile): sheen top stop, hue pool bottom, cream rim
// + dark outline, soft puddle shadow, dark-ink Phosphor glyph. Apps render
// over bare screen background — the wallpaper belongs to the launcher — so
// the placeholder carries its own quiet light field (duotone + light dome)
// instead of a dead-flat fill. The card floats above it, centered.

static void soon_create(lv_obj_t* root) {
    const AppDef* d = shell_foreground();
    if (!d) return;

    const int W = board_caps().width;
    const int H = board_caps().height;
    const bool small = (W < 400);
    const bool tiny  = (W < 300);   // 1.54" square LCD: different budget

    // ---- Quiet light field (the glass needs light to bend) -----------------
    // VER duotone from the wallpaper's own tokens, warm up top, falloff into
    // the navy floor ~80% down — the same anti-banding shape the launcher
    // wallpaper uses (short span, then held floor for AMOLED battery).
    lv_obj_set_style_bg_color(root, lv_color_mix(THEME_BG_MID, THEME_BG_TOP, 60), 0);
    lv_obj_set_style_bg_grad_color(root, THEME_BG_FLOOR, 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(root, 0, 0);
    lv_obj_set_style_bg_grad_stop(root, 205, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    // The launcher's luminous light dome: a giant cream ellipse whose top
    // edge is off-panel, lower rim arcing across the field. Drawn after the
    // duotone (sibling order = z-order) so it renders OVER it and THROUGH
    // the card's translucent glass — geography with no boundary lines.
    lv_obj_t* dome = lv_obj_create(root);
    lv_obj_set_size(dome, W + W * 2 / 3, H);
    lv_obj_set_pos(dome, -W / 4, -H * 5 / 8);
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

    // ---- The glass card ----------------------------------------------------
    const int card_w = tiny ? W - 32 : W - (small ? 64 : 112);
    const int card_h = tiny ? H - STATUSBAR_H - 44 : (small ? 264 : 288);
    const int radius = tiny ? 26 : (small ? 36 : 40);

    lv_obj_t* card = lv_obj_create(root);
    lv_obj_set_size(card, card_w, card_h);
    if (tiny) lv_obj_align(card, LV_ALIGN_TOP_MID, 0, STATUSBAR_H + 6);
    else      lv_obj_align(card, LV_ALIGN_CENTER, 0, -10);

    const lv_color_t hue  = lv_color_hex(d->tile_rgb);
    // Backdrop light behind the card rows — the duotone at mid-screen, in
    // wallpaper tokens (the pool stop must pull that light through, exactly
    // like the launcher's wall_base_at()).
    const lv_color_t wall = lv_color_mix(THEME_BG_FLOOR, THEME_BG_TOP, 80);

    // LIFT fill, one VER gradient carrying the whole material: top stop is
    // the sheen (wall lifted toward cream, hue as a cast), bottom stop the
    // pool (hue gathering, near-white kissed in). Brightness beats the
    // backdrop by a wide margin or the card would read as a hole.
    lv_obj_set_style_bg_color(card,
        lv_color_mix(lv_color_mix(wall, RIM_CREAM, 85), hue, 165), 0);
    lv_obj_set_style_bg_opa(card, 145, 0);
    lv_obj_set_style_bg_grad_color(card,
        lv_color_mix(lv_color_mix(wall, lv_color_hex(0xffffff), 12), hue, 82), 0);
    lv_obj_set_style_bg_grad_opa(card, 185, 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(card, 0, 0);
    lv_obj_set_style_bg_grad_stop(card, 240, 0);

    // Soft puddle shadow + cream rim + dark outer contour (launcher-tuned).
    lv_obj_set_style_shadow_color(card, GLASS_SHADOW, 0);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_spread(card, 0, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_opa(card, 25, 0);
    lv_obj_set_style_border_color(card, RIM_CREAM, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_opa(card, 95, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_outline_color(card, GLASS_SHADOW, 0);
    lv_obj_set_style_outline_width(card, 1, 0);
    lv_obj_set_style_outline_pad(card, 0, 0);
    lv_obj_set_style_outline_opa(card, 90, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Directional rim: clipped sheen crest across the card's top — light
    // comes from above, so the glint sits on the upper arc only. Slim on a
    // card so it reads as the reference bar's top sheen, not a mid band.
    const int glint_h = card_h * 3 / 8 - 12;
    lv_obj_t* glint = lv_obj_create(card);
    lv_obj_set_size(glint, card_w - 32, glint_h);
    lv_obj_align(glint, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_color(glint, lv_color_mix(RIM_CREAM, lv_color_hex(0xfff6ea), 50), 0);
    lv_obj_set_style_bg_grad_color(glint, RIM_CREAM, 0);
    lv_obj_set_style_bg_grad_dir(glint, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(glint, 72, 0);
    lv_obj_set_style_bg_grad_opa(glint, 0, 0);
    lv_obj_set_style_bg_main_stop(glint, 0, 0);
    lv_obj_set_style_bg_grad_stop(glint, 255, 0);
    lv_obj_set_style_radius(glint, glint_h / 2, 0);
    lv_obj_set_style_border_width(glint, 0, 0);
    lv_obj_set_style_pad_all(glint, 0, 0);
    lv_obj_set_style_clip_corner(glint, true, 0);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(glint, LV_OBJ_FLAG_IGNORE_LAYOUT);

    // Glyph: dark ink on the sheen crest (bar-glass-buttons anatomy).
    lv_obj_t* glyph = lv_label_create(card);
    lv_label_set_text(glyph, d->glyph);
    lv_obj_set_style_text_font(glyph, small ? &font_phos_28 : &font_phos_40, 0);
    lv_obj_set_style_text_color(glyph, THEME_INK, 0);
    lv_obj_set_style_text_opa(glyph, 235, 0);
    lv_obj_align(glyph, LV_ALIGN_TOP_MID, 0, tiny ? 14 : 30);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, d->title);
    lv_obj_set_style_text_font(title, &font_inter_28, 0);
    lv_obj_set_style_text_color(title, THEME_TEXT, 0);
    lv_obj_align_to(title, glyph, LV_ALIGN_OUT_BOTTOM_MID,
                    0, tiny ? 6 : (small ? 12 : 16));

    lv_obj_t* blurb = lv_label_create(card);
    lv_label_set_text(blurb, d->blurb ? d->blurb : "");
    lv_obj_set_style_text_font(blurb, &font_inter_16, 0);
    lv_obj_set_style_text_color(blurb, THEME_DIM, 0);
    lv_obj_set_width(blurb, card_w - (tiny ? 32 : 56));
    lv_obj_set_style_text_align(blurb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(blurb, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(blurb, title, LV_ALIGN_OUT_BOTTOM_MID,
                    0, tiny ? 6 : (small ? 8 : 12));

    // Status line: the accent tell that this is a promise, not a broken app.
    lv_obj_t* soon = lv_label_create(card);
    lv_label_set_text(soon, "Available in a future update");
    lv_obj_set_style_text_font(soon, &font_inter_14, 0);
    lv_obj_set_style_text_color(soon, THEME_ACCENT, 0);
    lv_obj_set_width(soon, card_w - (tiny ? 24 : 40));
    lv_obj_set_style_text_align(soon, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(soon, LV_LABEL_LONG_WRAP);
    lv_obj_align(soon, LV_ALIGN_BOTTOM_MID, 0, -(tiny ? 8 : (small ? 22 : 26)));
}

// `extern` is required: a file-scope `const` object has internal linkage in
// C++, so without it the registry in shell/apps.cpp can't see these symbols.
#define SOON_APP(sym, ident, name, icon, rgb, caps, text)  \
    extern const AppDef sym = {                            \
        .id = ident, .title = name, .glyph = icon,         \
        .tile_rgb = rgb, .blurb = text,                    \
        .required_caps = caps,                             \
        .persistent = false, .immersive = false,           \
        .create = soon_create, .destroy = nullptr,         \
        .tick = nullptr, .on_button = nullptr,             \
        .on_usage = nullptr, .on_ble = nullptr, .on_battery = nullptr, \
    }

SOON_APP(app_hostmon, "hostmon", "Host", PH_GLOBE, 0x7a4b6b,
         APP_CAP_WIFI,
         "Your dev machine's CPU, memory and network at a glance.");

// Settings now lives in apps/settings/app.cpp.