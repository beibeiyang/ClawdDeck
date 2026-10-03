#include "../shell/app.h"
#include "phosphor_cp.h"
#include "../shell/shell.h"
#include "../theme.h"
#include "../hal/board_caps.h"
#include "../shell/statusbar.h"

LV_FONT_DECLARE(font_inter_28);
LV_FONT_DECLARE(font_inter_16);
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

static void soon_create(lv_obj_t* root) {
    const AppDef* d = shell_foreground();
    if (!d) return;

    const int W = board_caps().width;
    const int H = board_caps().height;
    const bool small = (W < 400);

    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t* badge = lv_obj_create(root);
    const int edge = small ? 72 : 96;
    lv_obj_set_size(badge, edge, edge);
    lv_obj_set_style_bg_color(badge, lv_color_hex(d->tile_rgb), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, edge * 28 / 100, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, STATUSBAR_H + (small ? 20 : 40));

    lv_obj_t* glyph = lv_label_create(badge);
    lv_label_set_text(glyph, d->glyph);
    lv_obj_set_style_text_font(glyph, small ? &font_phos_28
                                            : &font_phos_40, 0);
    lv_obj_set_style_text_color(glyph, THEME_INK, 0);
    lv_obj_center(glyph);

    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, d->title);
    lv_obj_set_style_text_font(title, &font_inter_28, 0);
    lv_obj_set_style_text_color(title, THEME_TEXT, 0);
    lv_obj_align_to(title, badge, LV_ALIGN_OUT_BOTTOM_MID, 0, small ? 14 : 24);

    lv_obj_t* blurb = lv_label_create(root);
    lv_label_set_text(blurb, d->blurb ? d->blurb : "");
    lv_obj_set_style_text_font(blurb, &font_inter_16, 0);
    lv_obj_set_style_text_color(blurb, THEME_DIM, 0);
    lv_obj_set_width(blurb, W - (small ? 40 : 80));
    lv_obj_set_style_text_align(blurb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(blurb, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(blurb, title, LV_ALIGN_OUT_BOTTOM_MID, 0, small ? 8 : 16);

    lv_obj_t* soon = lv_label_create(root);
    lv_label_set_text(soon, "Not built yet");
    lv_obj_set_style_text_font(soon, &font_inter_16, 0);
    lv_obj_set_style_text_color(soon, THEME_ACCENT, 0);
    lv_obj_align(soon, LV_ALIGN_BOTTOM_MID, 0, -(H / 8));
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
