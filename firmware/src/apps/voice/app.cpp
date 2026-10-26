#include "../../shell/app.h"
#include "../../shell/statusbar.h"
#include "../../theme.h"
#include "../../phosphor_cp.h"
#include "../../ble.h"
#include "../../hal/input_hal.h"
#include "../../hal/board_caps.h"
#include "../../hal/audio_in_hal.h"
#include "voice_layout.h"
#include <Arduino.h>
#include <ArduinoJson.h>

LV_FONT_DECLARE(font_inter_20);
LV_FONT_DECLARE(font_inter_32);
LV_FONT_DECLARE(font_inter_16);
LV_FONT_DECLARE(font_inter_14);
LV_FONT_DECLARE(font_phos_28);
LV_FONT_DECLARE(font_phos_64);

// Voice input over the existing HID and dictation transports.

#define VOICE_FOOT_H 34
#define HID_SPACE 0x2C

enum voice_state_t {
    VOICE_IDLE,
    VOICE_RECORDING,       // PTT path: HID Space held (drive the Mac's voice mode)
    VOICE_PROCESSING,      // PTT path: waiting out the Mac's pipeline
    VOICE_DICTATING,       // dictate path: mic → ADPCM → BLE → daemon STT (live 'vp' text)
    VOICE_TRANSCRIBING,    // dictate path: EOU sent, waiting for 'vt' reply
};

static lv_obj_t* mic_btn    = nullptr;
static lv_obj_t* mic_glyph  = nullptr;
static lv_obj_t* state_lbl  = nullptr;
static lv_obj_t* level_bar  = nullptr;
static lv_obj_t* transcript = nullptr;

static voice_state_t state = VOICE_IDLE;
static bool          ptt_via_primary = false;
static uint32_t      processing_at = 0;

// Dictate stream state (mic tile hold): ADPCM blocks → ble_voice_send_chunk.
static uint8_t  dict_seq        = 0;
static bool     mic_capture_ok  = false;
static uint32_t transcribe_from = 0;
static uint8_t  dict_block[179];     // negotiated ADPCM block awaiting a notify slot
static size_t   dict_block_len  = 0;
static uint8_t  dict_audio_flags = 0; // bit1 = 8 kHz; bit2 reserved for overrun on EOU
static uint32_t dict_sent_blocks = 0;

// ---- Live transcript ('vp' partials from the daemon) ------------------------
// The daemon sends {"t":"vp","c":<committed>,"p":<pending>} a few times a
// second while you talk. Committed words never change; the pending tail is
// the model's current guess and renders dimmed. The transcript box holds only
// a few lines, so the oldest words are trimmed from the left to keep the
// newest speech visible on every panel size.
static size_t live_char_budget = 80;    // computed from the box in voice_create
#define LIVE_MAX_LINES 2                  // a glanceable ticker, not a document

// The usage hint lives in the transcript box. Live text, results and errors
// borrow the box briefly; the hint comes back after HINT_*_MS of idle so the
// panel never strands stale text.
#define VOICE_HINT "Hold the mic to dictate.\nHold BOOT for Mac voice mode."
#define HINT_AFTER_STATUS_MS      4000    // "Typed 12 words on your Mac"
#define HINT_AFTER_ERROR_MS       6000
#define HINT_AFTER_TRANSCRIPT_MS 15000    // display-only mode: time to read
static uint32_t hint_due_at = 0;          // 0 = nothing scheduled

// Measure how many characters of the transcript font fit the fixed box.
static size_t transcript_char_budget(const lv_font_t* font, int w, int h) {
    static const char sample[] = "the quick brown fox jumps over a lazy dog";
    lv_point_t sz;
    lv_text_get_size(&sz, sample, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int avg_px = sz.x > 0 ? sz.x / (int)(sizeof(sample) - 1) : 8;
    const int line_h = lv_font_get_line_height(font) + 2;
    int lines        = line_h > 0 ? h / line_h : 2;
    if (lines > LIVE_MAX_LINES) lines = LIVE_MAX_LINES;
    const int cpl    = avg_px > 0 ? w / avg_px : 40;
    const int budget = cpl * lines * 85 / 100;   // wrap slack: lines break at words
    return budget > 24 ? (size_t)budget : 24;
}

// Copy src into dst minus LVGL recolor markers ('#' toggles color runs).
static size_t copy_plain(char* dst, size_t cap, const char* src) {
    size_t o = 0;
    for (; src && *src && o + 1 < cap; src++) {
        if (*src != '#') dst[o++] = *src;
    }
    dst[o] = '\0';
    return o;
}

static void show_live(const char* committed, const char* pending) {
    if (!transcript) return;
    hint_due_at = 0;
    static char c[200], p[100], out[340];
    size_t cn = copy_plain(c, sizeof(c), committed);
    const size_t pn = copy_plain(p, sizeof(p), pending);

    // Trim old committed words so committed + pending fit the box.
    const char* cstart = c;
    const size_t room = live_char_budget > pn + 1 ? live_char_budget - pn - 1 : 0;
    bool trimmed = false;
    if (cn > room) {
        cstart = c + (cn - room);
        const char* sp = strchr(cstart, ' ');
        if (sp && sp - cstart < 20) cstart = sp + 1;
        cn = strlen(cstart);
        trimmed = true;
    }
    // Dim gray for the unconfirmed tail via LVGL inline recolor.
    snprintf(out, sizeof(out), "%s%s%s%s%s%s",
             trimmed ? "..." : "", cstart,
             (cn && pn) ? " " : "",
             pn ? "#7c8288 " : "", p, pn ? "#" : "");
    lv_label_set_recolor(transcript, true);
    lv_obj_set_style_text_color(transcript, THEME_TEXT, 0);
    lv_label_set_text(transcript, out);
}

static void show_message(const char* msg, bool final_text) {
    if (!transcript) return;
    hint_due_at = 0;
    lv_label_set_recolor(transcript, false);
    lv_obj_set_style_text_color(transcript, final_text ? THEME_TEXT : THEME_DIM, 0);
    lv_label_set_text(transcript, msg);
}

static void show_hint(void) {
    show_message(VOICE_HINT, false);
}

// Show msg now, then fall back to the usage hint after delay_ms.
static void show_then_hint(const char* msg, bool final_text, uint32_t delay_ms) {
    show_message(msg, final_text);
    hint_due_at = lv_tick_get() + delay_ms;
    if (!hint_due_at) hint_due_at = 1;   // 0 means "none"
}

// Send every captured block that's ready. A block whose notify fails (link
// busy) is kept and retried first next time, so nothing is dropped here.
static void dictate_pump(void) {
    for (int i = 0; i < 32; i++) {   // bound one tick's work
        if (!dict_block_len)
            dict_block_len = audio_in_hal_read_adpcm(dict_block, sizeof(dict_block));
        if (!dict_block_len) return;
        if (!ble_voice_send_chunk(dict_seq, dict_audio_flags,
                                  dict_block, (uint8_t)dict_block_len))
            return;
        dict_seq++;
        dict_sent_blocks++;
        dict_block_len = 0;
    }
}

// Stop the mic, flush every block captured before the stop, then close the
// stream with the EOU marker. Retries are bounded (~3 s) so a dead link can't
// hang the UI; a lost EOU would leave the daemon waiting forever.
static void dictate_finish(void) {
    audio_in_hal_stop();
    for (int i = 0; i < 600; i++) {
        dictate_pump();
        if (!dict_block_len) {
            dict_block_len = audio_in_hal_read_adpcm(dict_block, sizeof(dict_block));
            if (!dict_block_len) break;   // queue empty: tail flushed
        }
        delay(5);
    }
    audio_in_voice_stats_t stats = {};
    audio_in_hal_voice_stats(&stats);
    const uint8_t eou_flags = (uint8_t)(dict_audio_flags | 0x01 |
                              (stats.dropped_blocks ? 0x04 : 0x00));
    for (int i = 0; i < 100; i++) {
        if (ble_voice_send_chunk(dict_seq, eou_flags, nullptr, 0)) break;
        delay(5);
    }
    Serial.printf("voice: mtu=%u rate=%lu block_samples=%u captured=%lu sent=%lu "
                  "dropped=%lu queue_peak=%u\n",
                  ble_voice_mtu(),
                  (unsigned long)audio_in_hal_voice_sample_rate(),
                  stats.samples_per_block,
                  (unsigned long)stats.captured_blocks,
                  (unsigned long)dict_sent_blocks,
                  (unsigned long)stats.dropped_blocks,
                  stats.queue_high_water);
    dict_seq++;
    dict_block_len = 0;
}

static void set_state(voice_state_t next) {
    state = next;
    if (!state_lbl) return;

    switch (state) {
    case VOICE_IDLE:
        lv_label_set_text(state_lbl, "READY");
        if (level_bar) {
            lv_bar_set_value(level_bar, 0, LV_ANIM_OFF);
            lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (mic_btn) {
            lv_obj_set_style_bg_color(mic_btn, lv_color_hex(0x202326), 0);
            lv_obj_set_style_border_color(mic_btn, lv_color_hex(0x4a4f54), 0);
        }
        break;
    case VOICE_RECORDING:
        lv_label_set_text(state_lbl, "MAC VOICE");
        // The board is only sending HID Space here; it is not measuring the
        // Mac microphone, so do not imply that this is a live input meter.
        if (level_bar) lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        if (mic_btn) {
            lv_obj_set_style_bg_color(mic_btn, THEME_ACCENT, 0);
            lv_obj_set_style_border_color(mic_btn, lv_color_hex(0xf2a184), 0);
        }
        break;
    case VOICE_PROCESSING:
        lv_label_set_text(state_lbl, "PROCESSING");
        if (level_bar) lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        processing_at = lv_tick_get();
        break;
    case VOICE_DICTATING:
        lv_label_set_text(state_lbl, "LISTENING");
        if (level_bar) lv_obj_clear_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        if (mic_btn) {
            lv_obj_set_style_bg_color(mic_btn, THEME_ACCENT, 0);
            lv_obj_set_style_border_color(mic_btn, lv_color_hex(0xf2a184), 0);
        }
        break;
    case VOICE_TRANSCRIBING:
        lv_label_set_text(state_lbl, "TRANSCRIBING");
        if (level_bar) lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        processing_at = lv_tick_get();
        break;
    }
}

static void ptt_start(void) {
    if (state == VOICE_RECORDING) return;
    if (ble_get_state() != BLE_STATE_CONNECTED) {
        show_then_hint("Connect to your Mac first.", false, HINT_AFTER_ERROR_MS);
        return;
    }
    ble_keyboard_press(HID_SPACE, 0);
    set_state(VOICE_RECORDING);
    show_message("", false);
}

static void ptt_stop(void) {
    if (state != VOICE_RECORDING) return;
    ble_keyboard_release();
    set_state(VOICE_PROCESSING);
}

// ---- Dictate-to-panel (mic tile hold): capture → chunk → daemon whisper ----
static void dictate_start(void) {
    if (state != VOICE_IDLE) return;
    if (ble_get_state() != BLE_STATE_CONNECTED) {
        show_then_hint("Connect to your Mac first.", false, HINT_AFTER_ERROR_MS);
        return;
    }
    const size_t payload_capacity = ble_voice_payload_capacity();
    if (!payload_capacity) {
        show_then_hint("Voice daemon not ready.", false, HINT_AFTER_ERROR_MS);
        return;
    }
    mic_capture_ok = audio_in_hal_start_voice(payload_capacity);
    if (!mic_capture_ok) {
        show_then_hint("Bluetooth audio link is too small.", false, HINT_AFTER_ERROR_MS);
        return;
    }
    dict_seq       = 0;
    dict_block_len = 0;
    dict_sent_blocks = 0;
    dict_audio_flags = audio_in_hal_voice_sample_rate() == 8000 ? 0x02 : 0x00;
    set_state(VOICE_DICTATING);
    show_message("", false);
}

static void dictate_stop(void) {
    if (state != VOICE_DICTATING) return;
    dictate_finish();
    set_state(VOICE_TRANSCRIBING);
}

// Hold-to-talk touch semantics: press-and-hold = Space down, release = Space
// up (ptt_via_primary means the BOOT button owns the press — see
// voice_on_button/voice_tick; that path must not double-handle releases).
// The mic tile itself = DICTATE now (hold): PTT stays on BOOT per the design
// (voice_on_button). The pressed LOOK lives in LV_STATE_PRESSED styles.
// The touch controller reports brief releases while a finger is held, so a
// release only ends dictation once it has lasted RELEASE_DEBOUNCE_MS; a
// re-press inside that window continues the same utterance.
#define RELEASE_DEBOUNCE_MS 250
static bool     release_pending = false;
static uint32_t release_at      = 0;

static void mic_event(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if (release_pending) release_pending = false;   // flicker: keep recording
        else dictate_start();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (state == VOICE_DICTATING && !release_pending) {
            release_pending = true;
            release_at      = lv_tick_get();
        }
    }
}

static void voice_create(lv_obj_t* root) {
    const int W = board_caps().width;
    const int H = board_caps().height;
    const bool compact = (W < 400);
    const VoiceLayout layout = voice_layout(W, H, STATUSBAR_H, VOICE_FOOT_H);
    const int btn = layout.mic.w;

    // Precision Black: a uniform field avoids RGB565 gradient banding.
    lv_obj_set_style_bg_color(root, lv_color_hex(0x050607), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, "Voice");
    lv_obj_set_style_text_font(title, &font_inter_32, 0);
    lv_obj_set_style_text_color(title, THEME_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUSBAR_H + (compact ? 8 : 14));

    // ---- Microphone control -------------------------------------------------
    mic_btn = lv_btn_create(root);
    lv_obj_set_size(mic_btn, btn, btn);
    lv_obj_align(mic_btn, LV_ALIGN_CENTER, 0, -6);

    // The button uses solid graphite in its resting state. State changes add
    // the single warm listening accent without moving any content.
    lv_obj_set_style_bg_color(mic_btn, lv_color_hex(0x202326), 0);
    lv_obj_set_style_bg_grad_dir(mic_btn, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_grad_opa(mic_btn, 0, 0);
    lv_obj_set_style_bg_opa(mic_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(mic_btn, lv_color_hex(0x4a4f54), 0);
    lv_obj_set_style_border_opa(mic_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mic_btn, 1, 0);
    lv_obj_set_style_shadow_width(mic_btn, 0, 0);

    // A quiet graphite rim defines the touch target without a glass effect.
    lv_obj_set_style_radius(mic_btn, btn / 2, 0);
    lv_obj_clear_flag(mic_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(mic_btn, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_bg_color(mic_btn, lv_color_hex(0x292d31), LV_STATE_PRESSED);

    lv_obj_add_event_cb(mic_btn, mic_event, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(mic_btn, mic_event, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(mic_btn, mic_event, LV_EVENT_PRESS_LOST, nullptr);

    // Warm white glyph, optically centered.
    mic_glyph = lv_label_create(mic_btn);
    lv_label_set_text(mic_glyph, PH_MICROPHONE);
    lv_obj_set_style_text_font(mic_glyph,
                               compact ? &font_phos_28 : &font_phos_64, 0);
    lv_obj_set_style_text_color(mic_glyph, lv_color_hex(0xfaf9f5), 0);
    lv_obj_set_style_text_opa(mic_glyph, 235, 0);
    lv_obj_set_style_text_letter_space(mic_glyph, 0, 0);
    lv_obj_align(mic_glyph, LV_ALIGN_CENTER, 0, -2);

    state_lbl = lv_label_create(root);
    lv_label_set_text(state_lbl, "Hold to talk");
    lv_obj_set_style_text_font(state_lbl, &font_inter_20, 0);
    lv_obj_set_style_text_color(state_lbl, THEME_DIM, 0);
    lv_obj_set_width(state_lbl, btn);
    lv_obj_set_style_text_align(state_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(state_lbl, mic_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 18);

    // Geometry is computed once from the display and home-pill keep-out.
    // Later state text can wrap or clip only inside its own fixed zone.
    lv_obj_set_size(mic_btn, layout.mic.w, layout.mic.h);
    lv_obj_set_align(mic_btn, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(mic_btn, layout.mic.x, layout.mic.y);
    lv_obj_set_size(state_lbl, layout.state.w, layout.state.h);
    lv_obj_set_align(state_lbl, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(state_lbl, layout.state.x, layout.state.y);
    lv_obj_set_style_text_font(state_lbl, compact ? &font_inter_16 : &font_inter_20, 0);
    lv_obj_set_style_text_letter_space(state_lbl, 1, 0);
    lv_label_set_long_mode(state_lbl, LV_LABEL_LONG_CLIP);

    level_bar = lv_bar_create(root);
    lv_obj_set_width(level_bar, btn);
    lv_obj_set_height(level_bar, 6);
    lv_bar_set_range(level_bar, 0, 100);
    lv_obj_set_style_bg_color(level_bar, THEME_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_radius(level_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(level_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(level_bar, THEME_GREEN, LV_PART_INDICATOR);
    lv_obj_align_to(level_bar, state_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_obj_set_size(level_bar, layout.meter.w, layout.meter.h);
    lv_obj_set_align(level_bar, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(level_bar, layout.meter.x, layout.meter.y);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(0x292d31), LV_PART_MAIN);
    lv_obj_set_style_bg_color(level_bar, THEME_ACCENT, LV_PART_INDICATOR);

    transcript = lv_label_create(root);
    lv_label_set_text(transcript, VOICE_HINT);
    lv_obj_set_style_text_font(transcript,
                               compact ? &font_inter_14 : &font_inter_16, 0);
    lv_obj_set_style_text_color(transcript, THEME_DIM, 0);
    lv_obj_set_style_text_opa(transcript, 200, 0);
    lv_obj_set_width(transcript, W - (compact ? 32 : 48));
    lv_obj_set_size(transcript, layout.transcript.w, layout.transcript.h);
    lv_label_set_long_mode(transcript, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(transcript, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(transcript, LV_ALIGN_BOTTOM_MID, 0, -(VOICE_FOOT_H + 12));
    lv_obj_set_align(transcript, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(transcript, layout.transcript.x, layout.transcript.y);
    lv_label_set_long_mode(transcript, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(transcript, compact ? 1 : 2, 0);
    live_char_budget = transcript_char_budget(compact ? &font_inter_14 : &font_inter_16,
                                              layout.transcript.w, layout.transcript.h);

    set_state(VOICE_IDLE);
}

static void voice_destroy(void) {
    hint_due_at = 0;
    // A dictation cut short by leaving the app: stop the mic + close the
    // chunk stream so the daemon doesn't wait on an EOU that never comes.
    if (state == VOICE_DICTATING) dictate_finish();
    release_pending = false;
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
    if (hint_due_at && state == VOICE_IDLE &&
        (int32_t)(lv_tick_get() - hint_due_at) >= 0) {
        show_hint();
    }

    // PTT (BOOT-held) release detection — unchanged.
    if (ptt_via_primary && state == VOICE_RECORDING &&
        !input_hal_is_held(INPUT_BTN_PRIMARY)) {
        ptt_via_primary = false;
        ptt_stop();
    }

    // The local meter is only shown during actual on-device dictation.
    if (state == VOICE_DICTATING && level_bar) {
        lv_bar_set_value(level_bar, audio_in_hal_peak(), LV_ANIM_OFF);
    }

    // Dictate chunk pump: capture runs in real time on its own; drain it.
    if (state == VOICE_DICTATING && mic_capture_ok) dictate_pump();
    if (release_pending && lv_tick_elaps(release_at) >= RELEASE_DEBOUNCE_MS) {
        release_pending = false;
        dictate_stop();
    }

    // Daemon replies over the JSON bridge: live 'vp' partials while dictating
    // (and any that land just after release), then the final 'vt' or an 'err'.
    if (state == VOICE_DICTATING || state == VOICE_TRANSCRIBING) {
        const char* rx = ble_bridge_take_rx();
        JsonDocument doc;   // allocates only when a message is parsed
        if (rx && deserializeJson(doc, rx) == DeserializationError::Ok) {
            const char* typ = doc["t"] | "";
            if (strcmp(typ, "vp") == 0) {
                show_live(doc["c"] | "", doc["p"] | "");
            } else if (state == VOICE_TRANSCRIBING && strcmp(typ, "vt") == 0) {
                set_state(VOICE_IDLE);
                // "inj" = what the Mac did with the text. When it was typed
                // into the focused app the panel needs only a one-line
                // receipt (the full text is on the Mac). Display-only mode
                // ("off") or an older daemon (no "inj") shows the transcript.
                const char* inj = doc["inj"] | "off";
                const char* m   = doc["m"] | "";
                if (strcmp(inj, "off") == 0 || !*m) {
                    show_then_hint(doc["tx"] | "", true, HINT_AFTER_TRANSCRIPT_MS);
                } else {
                    const bool ok = strcmp(inj, "ok") == 0;
                    show_then_hint(m, ok, ok ? HINT_AFTER_STATUS_MS : HINT_AFTER_ERROR_MS);
                }
            } else if (state == VOICE_TRANSCRIBING && strcmp(typ, "err") == 0) {
                set_state(VOICE_IDLE);
                // Show the daemon's message ("m"); generic text if it has none.
                const char* m = doc["m"] | "";
                show_then_hint(*m ? m : "Host error - see daemon log.", false,
                               HINT_AFTER_ERROR_MS);
            }
        }
        if (state == VOICE_TRANSCRIBING &&
            lv_tick_elaps(processing_at) > 15000) {  // 15s no reply = give up
            set_state(VOICE_IDLE);
            show_then_hint("Transcription timed out.", false, HINT_AFTER_ERROR_MS);
        }
    }

    if (state == VOICE_PROCESSING && processing_at != 0 &&
        lv_tick_elaps(processing_at) > 1200) {
        processing_at = 0;
        set_state(VOICE_IDLE);
        show_then_hint("Voice mode toggled on your Mac.", false, HINT_AFTER_STATUS_MS);
    }
}

extern const AppDef app_voice = {
    .id            = "voice",
    .title         = "Voice",
    .glyph         = PH_WAVEFORM,
    .tile_rgb      = 0x5f8f46,
    .blurb         = "Hold to talk. Live speech-to-text on your Mac, typed straight into Claude Code.",
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
