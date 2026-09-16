#include "../../hal/audio_in_hal.h"

bool audio_in_hal_start(void) { return false; }
void audio_in_hal_stop(void) {}
bool audio_in_hal_active(void) { return false; }
uint8_t audio_in_hal_peak(void) { return 0; }
void audio_in_hal_tick(void) {}
size_t audio_in_hal_read_stereo(int16_t* out, size_t frames) {
    (void)out;
    (void)frames;
    return 0;
}
