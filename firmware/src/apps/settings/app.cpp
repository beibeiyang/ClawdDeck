#include "../../shell/app.h"
#include "../../shell/statusbar.h"
#include "../../theme.h"
#include "../../phosphor_cp.h"
#include "../../brightness.h"
#include "../../ble.h"
#include "../../hal/board_caps.h"

LV_FONT_DECLARE(font_inter_20);
LV_FONT_DECLARE(font_inter_16);
LV_FONT_DECLARE(font_inter_14);

// Settings.
//
// iOS grouped-inset table: rows are GLASS cards (the launcher-tile material,
// scaled down) sitting in inset groups over the shell's dark backdrop, with
// iOS list typography -- Inter labels, right-aligned dim values, letterspaced
// section headers. Only exposes what the firmware can actually change today:
// brightness (via the existing NVS-backed brightness module) and BLE bonds.
// The idle timeout is a compile-time constant in idle_cfg.h, so it's shown as
// information rather than offered as a control -- a row that looks tappable
// but silently does nothing is worse than no row.

// Space kept clear at the bottom for the shell's home pill.
#define SETTINGS_FOOT_H 34

// Clearing bonds drops the daemon's link and forces a re-pair, so it takes
// two taps. This is how long the armed state lingers before reverting.
#define CONFIRM_WINDOW_MS 4000

// ---- iOS grouped-list discipline -------------------------------------------
#define ROW_H        52     // row card height
#define INSET        12     // inset grouped table: page margin
#define GROUP_GAP    12     // gap between groups (and header -> group)
#define ROW_GAP      8      // gap between rows inside one glass group
#define PAD_ROW      16     // horizontal padding inside a row card
#define HDR_LETTERS  2      // section-header letter-spacing (px)

static lv_obj_t* bright_val = nullptr;
static lv_obj_t* bond_val   = nullptr;
static lv_obj_t* unpair_lbl = nullptr;
static uint32_t  confirm_at = 0;   // 0 = not armed

static void set_bright_text(void) {
    if (bright_val)
        lv_label_set_text_fmt(bright_val, "%d%%",
                              (brightness_get() * 100) / 255);
}

static void set_bond_text(void) {
    if (bond_val)
        lv_label_set_text(bond_val, ble_has_bonds() ? "Paired" : "Not paired");
}

static void set_unpair_text(void) {
    if (!unpair_lbl) return;
    const bool armed = (confirm_at != 0);
    lv_label_set_text(unpair_lbl, armed ? "Tap again to confirm"
                                        : "Clear pairing");
    // Destructive action stays red in both states (iOS, SF Pro Regular);
    // the armed prompt only rewords. On glass like every other row.
    lv_obj_set_style_text_color(unpair_lbl, THEME_RED, 0);
}

static void brightness_cb(lv_event_t* e) {
    (void)e;
    brightness_cycle();
    set_bright_text();
}

static void unpair_cb(lv_event_t* e) {
    (void)e;
    if (confirm_at == 0) {
        confirm_at = lv_tick_get();
    } else {
        ble_clear_bonds();
        confirm_at = 0;
        set_bond_text();
    }
    set_unpair_text();
}

// ---- Type -------------------------------------------------------------------
static lv_obj_t* make_text(lv_obj_t* parent, const char* txt,
                           const lv_font_t* font, lv_opa_t opa) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    return l;
}

static void make_header(lv_obj_t* list, const char* text) {
    lv_obj_t* h = make_text(list, text, &font_inter_14, LV_OPA_COVER);
    lv_obj_set_style_text_color(h, THEME_DIM, 0);
    lv_obj_set_style_text_letter_space(h, HDR_LETTERS, 0);
}

// ---- Glass row material -----------------------------------------------------
// The house recipe (shell/launcher.cpp glass_tile, statusbar.cpp pill), scaled
// to a short pill. One fill carries the material: an OPAQUE premixed vertical
// gradient, warm cream at the sheen falling to the backdrop navy at the pool
// (compositing two independently alpha-graded layers bands at pill scale). Rim
// lives ON the object -- cream border + a 1px dark outline just outside to
// seat the glass on the dark backdrop -- so it cannot detach. Shadow is a soft
// low puddle, not a halo (width 26 / spread 2 / opa 40 read as glow pollution).
static void glass_row(lv_obj_t* row, int radius) {
    lv_obj_set_style_radius(row, radius, 0);
    lv_obj_set_style_bg_color(row, SHEEN_TINT, 0);
    lv_obj_set_style_bg_opa(row, 140, 0);
    lv_obj_set_style_bg_grad_color(row, THEME_BG, 0);
    lv_obj_set_style_bg_grad_opa(row, 80, 0);
    lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(row, 0, 0);
    lv_obj_set_style_bg_grad_stop(row, 255, 0);
    lv_obj_set_style_border_color(row, RIM_CREAM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_opa(row, 160, 0);
    lv_obj_set_style_outline_color(row, RIM_DARK, 0);
    lv_obj_set_style_outline_width(row, 1, 0);
    lv_obj_set_style_outline_pad(row, 0, 0);
    lv_obj_set_style_shadow_color(row, GLASS_SHADOW, 0);
    lv_obj_set_style_shadow_width(row, 14, 0);
    lv_obj_set_style_shadow_spread(row, 0, 0);
    lv_obj_set_style_shadow_ofs_y(row, 5, 0);
    lv_obj_set_style_shadow_opa(row, 25, 0);
    // Pressed glass feels thicker: brighter fill, deeper puddle.
    lv_obj_set_style_bg_opa(row, 200, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(row, 60, LV_STATE_PRESSED);
}

// Shared row plumbing: fixed-height glass card, label left / value right.
// Returns the value label so live rows can update it.
static lv_obj_t* row_content(lv_obj_t* row, const char* label,
                             const char* value) {
    lv_obj_set_style_pad_hor(row, PAD_ROW, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* name = make_text(row, label, &font_inter_20, LV_OPA_COVER);
    lv_obj_set_style_text_color(name, THEME_TEXT, 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* val = nullptr;
    if (value && value[0]) {
        val = make_text(row, value, &font_inter_16, 200);
        lv_obj_set_style_text_color(val, THEME_DIM, 0);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return val;
}

// One glass row. Returns the value label so live rows can update it.
static lv_obj_t* make_row(lv_obj_t* list, const char* label, const char* value,
                          lv_event_cb_t cb) {
    lv_obj_t* row = lv_obj_create(list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, ROW_H);
    glass_row(row, ROW_H / 2 - 4);
    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
    return row_content(row, label, value);
}

// A read-only group of rows sharing ONE glass card (iOS grouped-inset
// convention): transparent lines inside the card, hairline separators.
static void make_glass_group(lv_obj_t* list, const char** rows, int n) {
    lv_obj_t* card = lv_obj_create(list);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, n * ROW_H + (n - 1) * ROW_GAP);
    glass_row(card, 22);
    lv_obj_set_style_pad_hor(card, PAD_ROW, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    for (int i = 0; i < n; i++) {
        int y = i * ROW_H;
        lv_obj_t* line = lv_obj_create(card);
        lv_obj_set_width(line, lv_pct(100));
        lv_obj_set_height(line, ROW_H);
        lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_set_style_pad_all(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        row_content(line, rows[2 * i], rows[2 * i + 1]);
        if (i) {                       // hairline above lines after line 0
            lv_obj_t* sep = lv_obj_create(card);
            lv_obj_set_size(sep, lv_pct(100), 1);
            lv_obj_set_pos(sep, 0, y - 5);
            lv_obj_set_style_bg_color(sep, RIM_CREAM, 0);
            lv_obj_set_style_bg_opa(sep, 28, 0);
            lv_obj_set_style_border_width(sep, 0, 0);
            lv_obj_set_style_radius(sep, 0, 0);
            lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(sep, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(sep, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_move_to_index(sep, 0);
        }
    }
    // Stack z-order: separators under the line contents.
    for (int i = 0; i < n; i++) lv_obj_move_to_index(lv_obj_get_child(card, 0), n + i - (i > 0 ? 0 : 0));
}

static void settings_create(lv_obj_t* root) {
    const BoardCaps& c = board_caps();

    confirm_at = 0;

    // Opaque backdrop that carries the glass: the launcher's wallpaper hides
    // while an app is up, so the app paints its own slice of the same duotone
    // (deep base with the warm crest at the top) at full coverage.
    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(root, THEME_BG_TOP, 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(root, 80, 0);
    lv_obj_set_style_bg_grad_stop(root, 0, 0);

    // Inset grouped table.
    lv_obj_t* list = lv_obj_create(root);
    lv_obj_set_size(list, c.width - 2 * INSET,
                    c.height - STATUSBAR_H - SETTINGS_FOOT_H);
    lv_obj_set_pos(list, INSET, STATUSBAR_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, GROUP_GAP, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    // Display.
    make_header(list, "Display");
    bright_val = make_row(list, "Brightness", "", brightness_cb);
    set_bright_text();

    // Connection -- one glass card holding the read-only rows.
    make_header(list, "Connection");
    make_glass_group(list, nullptr, 3);
    lv_obj_t* card = lv_obj_get_child(list, lv_obj_get_child_cnt(list) - 1);
    lv_obj_t* line = lv_obj_get_child(card, 0);
    lv_obj_t* v    = lv_obj_get_child(line, 1);
    if (v) lv_label_set_text(v, ble_get_device_name());
    line = lv_obj_get_child(card, 2);
    v    = lv_obj_get_child(line, 1);
    if (v) lv_label_set_text(v, ble_get_mac_address());
    line = lv_obj_get_child(card, 4);
    bond_val = v ? lv_obj_get_child(line, 1) : nullptr;
    set_bond_text();

    // Destructive action: red text on the same glass as every other row.
    lv_obj_t* unpair = lv_obj_create(list);
    lv_obj_set_width(unpair, lv_pct(100));
    lv_obj_set_height(unpair, ROW_H);
    glass_row(unpair, ROW_H / 2 - 4);
    lv_obj_add_flag(unpair, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(unpair, unpair_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_pad_hor(unpair, PAD_ROW, 0);
    unpair_lbl = make_text(unpair, "", &font_inter_20, LV_OPA_COVER);
    lv_obj_set_style_text_color(unpair_lbl, THEME_RED, 0);
    lv_obj_align(unpair_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    set_unpair_text();

    // About -- one glass card holding the board facts.
    static char res_copy[24];
    snprintf(res_copy, sizeof(res_copy), "%dx%d", c.width, c.height);
    make_header(list, "About");
    make_glass_group(list, nullptr, 2);
    lv_obj_t* bcard = lv_obj_get_child(list, lv_obj_get_child_cnt(list) - 1);
    lv_obj_t* bline = lv_obj_get_child(bcard, 0);
    lv_obj_t* bv    = lv_obj_get_child(bline, 1);
    if (bv) lv_label_set_text(bv, c.name);
    bline = lv_obj_get_child(bcard, 2);
    bv    = bline ? lv_obj_get_child(bline, 1) : nullptr;
    if (bv) lv_label_set_text(bv, res_copy);
    make_row(list, "Built", __DATE__, nullptr);

    // iOS footer hint under the last group.
    lv_obj_t* foot = make_text(list,
        "Tap Brightness to cycle. Tap again to confirm clearing pairing.",
        &font_inter_14, 200);
    lv_obj_set_style_text_color(foot, THEME_DIM, 0);
    lv_obj_set_width(foot, lv_pct(100));
    lv_label_set_long_mode(foot, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(foot, 8, 0);
}

static void settings_destroy(void) {
    // The shell deletes the root, taking the widgets with it; drop the
    // cached pointers so a later tick can't touch freed objects.
    bright_val = nullptr;
    bond_val   = nullptr;
    unpair_lbl = nullptr;
    confirm_at = 0;
}

static void settings_tick(void) {
    if (confirm_at == 0) return;
    if (lv_tick_elaps(confirm_at) < CONFIRM_WINDOW_MS) return;
    confirm_at = 0;
    set_unpair_text();
}

static void settings_on_ble(ble_state_t state) {
    (void)state;
    set_bond_text();
}

// `extern` is required: a file-scope `const` object has internal linkage in
// C++, so without it the registry in shell/apps.cpp can't see this symbol.
extern const AppDef app_settings = {
    .id = "settings", .title = "Settings", .glyph = PH_GEAR,
    .tile_rgb = 0x3d3d3b,
    .blurb = "Brightness, pairing and board info.",
    .required_caps = APP_CAP_NONE,
    .persistent = false, .immersive = false,
    .create = settings_create, .destroy = settings_destroy,
    .tick = settings_tick, .on_button = nullptr,
    .on_usage = nullptr, .on_ble = settings_on_ble, .on_battery = nullptr,
};