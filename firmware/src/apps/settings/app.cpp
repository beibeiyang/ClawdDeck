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
// Only exposes what the firmware can actually change today: brightness (via
// the existing NVS-backed brightness module) and BLE bonds. The idle timeout
// is a compile-time constant in idle_cfg.h, so it's shown as information
// rather than offered as a control -- a row that looks tappable but silently
// does nothing is worse than no row.

// Space kept clear at the bottom for the shell's home pill.
#define SETTINGS_FOOT_H 34

// Clearing bonds drops the daemon's link and forces a re-pair, so it takes
// two taps. This is how long the armed state lingers before reverting.
#define CONFIRM_WINDOW_MS 4000

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
    lv_obj_set_style_text_color(unpair_lbl, armed ? THEME_RED : THEME_ACCENT, 0);
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

// One full-width row. Returns the value label so live rows can update it.
static lv_obj_t* make_row(lv_obj_t* list, const char* label, const char* value,
                          lv_event_cb_t cb) {
    lv_obj_t* row = lv_obj_create(list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    // Glass row card — the house recipe (launch-tile material, short form).
    lv_obj_set_style_bg_color(row, SHEEN_TINT, 0);
    lv_obj_set_style_bg_opa(row, 200, 0);
    lv_obj_set_style_bg_grad_color(row, THEME_BG, 0);
    lv_obj_set_style_bg_grad_opa(row, 80, 0);
    lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(row, 0, 0);
    lv_obj_set_style_bg_grad_stop(row, 255, 0);
    lv_obj_set_style_radius(row, 16, 0);
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
    lv_obj_set_style_bg_opa(row, 230, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_style_pad_ver(row, 14, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* name = lv_label_create(row);
    lv_label_set_text(name, label);
    lv_obj_set_style_text_font(name, &font_inter_20, 0);
    lv_obj_set_style_text_color(name, THEME_TEXT, 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

    if (!value) return nullptr;

    lv_obj_t* val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_font(val, &font_inter_16, 0);
    lv_obj_set_style_text_color(val, THEME_DIM, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
    return val;
}

static void make_header(lv_obj_t* list, const char* text) {
    lv_obj_t* h = lv_label_create(list);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_font(h, &font_inter_14, 0);
    lv_obj_set_style_text_color(h, THEME_DIM, 0);
    lv_obj_set_style_text_letter_space(h, 2, 0);
    lv_obj_set_style_pad_top(h, 6, 0);
}

static void settings_create(lv_obj_t* root) {
    const BoardCaps& c = board_caps();
    const int inset = (c.width >= 400) ? 24 : 12;

    confirm_at = 0;

    // App backdrop: its own slice of the launcher duotone (warm crest →
    // navy floor) so the glass rows carry wall light; dead black reads
    // as flat plastic under glass.
    lv_obj_set_style_bg_color(root, THEME_BG_TOP, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(root, THEME_BG_BOT, 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(root, 0, 0);
    lv_obj_set_style_bg_grad_stop(root, 230, 0);

    lv_obj_t* list = lv_obj_create(root);
    lv_obj_set_size(list, c.width - 2 * inset,
                    c.height - STATUSBAR_H - SETTINGS_FOOT_H);
    lv_obj_set_pos(list, inset, STATUSBAR_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    make_header(list, "Display");
    bright_val = make_row(list, "Brightness", "", brightness_cb);
    set_bright_text();

    make_header(list, "Connection");
    make_row(list, "Name", ble_get_device_name(), nullptr);
    make_row(list, "Address", ble_get_mac_address(), nullptr);
    bond_val = make_row(list, "Status", "", nullptr);
    set_bond_text();

    // The label is the row's own name, so it's tracked separately to be
    // retitled between "Clear pairing" and the armed confirm prompt.
    lv_obj_t* unpair = lv_obj_create(list);
    lv_obj_set_width(unpair, lv_pct(100));
    lv_obj_set_height(unpair, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(unpair, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(unpair, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(unpair, 12, 0);
    lv_obj_set_style_border_width(unpair, 0, 0);
    lv_obj_set_style_pad_hor(unpair, 16, 0);
    lv_obj_set_style_pad_ver(unpair, 14, 0);
    lv_obj_clear_flag(unpair, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(unpair, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(unpair, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(unpair, unpair_cb, LV_EVENT_CLICKED, NULL);

    unpair_lbl = lv_label_create(unpair);
    lv_obj_set_style_text_font(unpair_lbl, &font_inter_20, 0);
    lv_obj_align(unpair_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    set_unpair_text();

    make_header(list, "About");
    make_row(list, "Board", c.name, nullptr);

    char res[24];
    snprintf(res, sizeof(res), "%dx%d", c.width, c.height);
    make_row(list, "Display", res, nullptr);

    make_row(list, "Built", __DATE__, nullptr);
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
