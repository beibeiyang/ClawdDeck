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
