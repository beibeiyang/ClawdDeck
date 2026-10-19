#include "../../hal/audio_in_hal.h"

bool audio_in_hal_start(void) { return false; }
void audio_in_hal_stop(void) {}
bool audio_in_hal_active(void) { return false; }
uint8_t audio_in_hal_peak(void) { return 0; }
void audio_in_hal_tick(void) {}
bool audio_in_hal_start_voice(size_t) { return false; }
uint32_t audio_in_hal_voice_sample_rate(void) { return 16000; }
void audio_in_hal_voice_stats(audio_in_voice_stats_t* out) {
    if (out) *out = {};
}
size_t audio_in_hal_read_stereo(int16_t* out, size_t frames) {
    (void)out;
    (void)frames;
    return 0;
}

size_t audio_in_hal_read_adpcm(uint8_t* out, size_t cap) {
    (void)out;
    (void)cap;
    return 0;
}
