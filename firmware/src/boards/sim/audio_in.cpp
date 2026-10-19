#include "../../hal/audio_in_hal.h"
#include <lvgl.h>
#include <math.h>

static bool    active = false;
static uint8_t peak   = 0;
static float   phase  = 0.0f;

bool audio_in_hal_start(void) {
    active = true;
    peak   = 0;
    phase  = 0.0f;
    return true;
}

void audio_in_hal_stop(void) {
    active = false;
    peak   = 0;
}

bool audio_in_hal_active(void) { return active; }
uint8_t audio_in_hal_peak(void) { return peak; }

void audio_in_hal_tick(void) {
    if (!active) return;
    phase += 0.15f;
    peak = (uint8_t)(40 + (int)(sinf(phase) * 35.0f + 20.0f));
}

size_t audio_in_hal_read_stereo(int16_t* out, size_t frames) {
    if (!active || !out || frames == 0) return 0;
    for (size_t i = 0; i < frames; i++) {
        const float t = phase + (float)i * 0.02f;
        const int16_t v = (int16_t)(sinf(t * 3.7f) * 12000.0f +
                                    sinf(t * 0.9f) * 6000.0f);
        out[i * 2]     = v;
        out[i * 2 + 1] = v;
    }
    phase += (float)frames * 0.02f;
    return frames;
}

// Voice dictation on the sim: fold the sine to "mono" and emit it as a
// trivial 4-bit delta block (predictor state folded into the phase — the
// sim's payload exercises the transport + daemon plumbing, not the codec).
size_t audio_in_hal_read_adpcm(uint8_t* out, size_t cap) {
    if (!active || !out || cap < 4) return 0;
    // 120-sample block @ the sim phase: 60 bytes packed + 4-byte header
    const size_t n = 120;
    if (cap < 4 + n / 2) return 0;
    out[0] = 0; out[1] = 0; out[2] = 40; out[3] = 0;   // pred=0, step=40
    float t0 = phase;
    for (size_t i = 0; i < n; i++) {
        const float t = t0 + (float)i * 0.02f;
        const int16_t v = (int16_t)(sinf(t * 3.7f) * 12000.0f +
                                    sinf(t * 0.9f) * 6000.0f);
        // fake differential nibble: scale into 0..15
        const int nib = (v + 16000) * 15 / 32000;
        if (i & 1) out[4 + i / 2] |= (uint8_t)(nib << 4);
        else       out[4 + i / 2]  = (uint8_t)nib;
    }
    phase = t0 + (float)n * 0.02f;
    return 4 + n / 2;
}
