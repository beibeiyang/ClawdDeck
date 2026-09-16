#include "../../shell/app.h"
#include "../../shell/statusbar.h"
#include "../../theme.h"
#include "../../ble.h"
#include "../../hal/input_hal.h"
#include "../../hal/board_caps.h"
#include <Arduino.h>

LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);

// Push-to-talk into Claude Code via BLE HID Space. Whisper transcription and
// WiFi offload to the host bridge are not wired yet — this ships the UX shell
// plus a live level meter placeholder while the mic HAL lands.

#define VOICE_FOOT_H 34
#define HID_SPACE 0x2C

enum voice_state_t {
    VOICE_IDLE,
    VOICE_RECORDING,
    VOICE_PROCESSING,
};

static lv_obj_t* mic_btn    = nullptr;
static lv_obj_t* mic_glyph  = nullptr;
static lv_obj_t* state_lbl  = nullptr;
static lv_obj_t* level_bar  = nullptr;
static lv_obj_t* transcript = nullptr;

static voice_state_t state = VOICE_IDLE;
static bool          ptt_via_primary = false;
static uint32_t      processing_at = 0;

static void set_state(voice_state_t next) {
    state = next;
    if (!mic_btn || !state_lbl) return;

    switch (state) {
    case VOICE_IDLE:
        lv_label_set_text(state_lbl, "Hold to talk");
        lv_obj_set_style_bg_color(mic_btn, THEME_PANEL, 0);
        lv_obj_set_style_text_color(mic_glyph, THEME_ACCENT, 0);
        if (level_bar) lv_bar_set_value(level_bar, 0, LV_ANIM_OFF);
        break;
    case VOICE_RECORDING:
        lv_label_set_text(state_lbl, "Listening…");
        lv_obj_set_style_bg_color(mic_btn, THEME_RED, 0);
        lv_obj_set_style_text_color(mic_glyph, THEME_TEXT, 0);
        break;
    case VOICE_PROCESSING:
        lv_label_set_text(state_lbl, "Processing…");
        lv_obj_set_style_bg_color(mic_btn, THEME_PANEL, 0);
        lv_obj_set_style_text_color(mic_glyph, THEME_DIM, 0);
        processing_at = lv_tick_get();
        break;
    }
}

static void ptt_start(void) {
    if (state == VOICE_RECORDING) return;
    if (ble_get_state() != BLE_STATE_CONNECTED) {
        if (transcript)
            lv_label_set_text(transcript, "Connect to your Mac first.");
        return;
    }
    ble_keyboard_press(HID_SPACE, 0);
    set_state(VOICE_RECORDING);
    if (transcript) lv_label_set_text(transcript, "");
}

static void ptt_stop(void) {
    if (state != VOICE_RECORDING) return;
    ble_keyboard_release();
    set_state(VOICE_PROCESSING);
}

static void mic_event(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        ptt_via_primary = false;
        ptt_start();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!ptt_via_primary) ptt_stop();
    }
}

static void voice_create(lv_obj_t* root) {
    const int W = board_caps().width;
    const bool compact = (W < 400);
    const int btn = compact ? 120 : 160;

    lv_obj_set_style_bg_color(root, THEME_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, "Voice");
    lv_obj_set_style_text_font(title, &font_styrene_28, 0);
    lv_obj_set_style_text_color(title, THEME_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUSBAR_H + (compact ? 8 : 16));

    mic_btn = lv_btn_create(root);
    lv_obj_set_size(mic_btn, btn, btn);
    lv_obj_set_style_radius(mic_btn, btn / 2, 0);
    lv_obj_set_style_border_width(mic_btn, 0, 0);
    lv_obj_align(mic_btn, LV_ALIGN_CENTER, 0, -8);
    lv_obj_add_event_cb(mic_btn, mic_event, LV_EVENT_ALL, nullptr);

    mic_glyph = lv_label_create(mic_btn);
    lv_label_set_text(mic_glyph, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(mic_glyph,
                               compact ? &lv_font_montserrat_28
                                       : &lv_font_montserrat_40, 0);
    lv_obj_center(mic_glyph);

    state_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(state_lbl, &font_styrene_16, 0);
    lv_obj_set_style_text_color(state_lbl, THEME_DIM, 0);
    lv_obj_align_to(state_lbl, mic_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

    level_bar = lv_bar_create(root);
    lv_obj_set_width(level_bar, btn);
    lv_obj_set_height(level_bar, 6);
    lv_bar_set_range(level_bar, 0, 100);
    lv_obj_set_style_bg_color(level_bar, THEME_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_radius(level_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(level_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(level_bar, THEME_GREEN, LV_PART_INDICATOR);
    lv_obj_align_to(level_bar, state_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    transcript = lv_label_create(root);
    lv_label_set_text(transcript,
                      "BOOT also works as push-to-talk here.\n"
                      "Whisper + reply display need the host bridge.");
    lv_obj_set_style_text_font(transcript, &font_styrene_14, 0);
    lv_obj_set_style_text_color(transcript, THEME_DIM, 0);
    lv_obj_set_width(transcript, W - (compact ? 32 : 48));
    lv_label_set_long_mode(transcript, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(transcript, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(transcript, LV_ALIGN_BOTTOM_MID, 0, -(VOICE_FOOT_H + 12));

    set_state(VOICE_IDLE);
}

static void voice_destroy(void) {
    mic_btn = nullptr;
    mic_glyph = nullptr;
    state_lbl = nullptr;
    level_bar = nullptr;
    transcript = nullptr;
    ptt_via_primary = false;
    state = VOICE_IDLE;
}

static bool voice_on_button(app_btn_t btn) {
    if (btn != APP_BTN_PRIMARY) return false;
    ptt_via_primary = true;
    ptt_start();
    return true;
}

static void voice_tick(void) {
    if (ptt_via_primary && state == VOICE_RECORDING &&
        !input_hal_is_held(INPUT_BTN_PRIMARY)) {
        ptt_via_primary = false;
        ptt_stop();
    }

    if (state == VOICE_RECORDING && level_bar) {
        // Placeholder meter until audio_in HAL feeds real peaks.
        const int v = 25 + (int)(lv_tick_get() / 40 % 60);
        lv_bar_set_value(level_bar, v, LV_ANIM_OFF);
    }

    if (state == VOICE_PROCESSING && processing_at != 0 &&
        lv_tick_elaps(processing_at) > 1200) {
        processing_at = 0;
        set_state(VOICE_IDLE);
        if (transcript) {
            lv_label_set_text(transcript,
                              "Voice mode toggled on your Mac.\n"
                              "Transcription display needs the host bridge.");
        }
    }
}

extern const AppDef app_voice = {
    .id            = "voice",
    .title         = "Voice",
    .glyph         = LV_SYMBOL_AUDIO,
    .tile_rgb      = 0x788c5d,
    .blurb         = "Hold to talk. Whisper runs on your Mac and the text goes straight into Claude Code.",
    .required_caps = APP_CAP_MIC | APP_CAP_WIFI | APP_CAP_PSRAM,
    .persistent    = false,
    .immersive     = false,
    .create        = voice_create,
    .destroy       = voice_destroy,
    .tick          = voice_tick,
    .on_button     = voice_on_button,
    .on_usage      = nullptr,
    .on_ble        = nullptr,
    .on_battery    = nullptr,
};
