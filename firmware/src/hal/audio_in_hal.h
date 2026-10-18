#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Microphone capture for Mic Scope and (eventually) Voice. Boards without a
// mic path no-op and audio_in_hal_start() returns false.

bool    audio_in_hal_start(void);
void    audio_in_hal_stop(void);
bool    audio_in_hal_active(void);
uint8_t audio_in_hal_peak(void);   // 0..100, smoothed RMS/peak
void    audio_in_hal_tick(void);   // poll I2S while the capture app is open

// Stereo interleaved samples @ 16 kHz for SpecAnalyzer FFT. Returns frames read.
size_t audio_in_hal_read_stereo(int16_t* out, size_t frames);

// ---- Voice dictation capture (dicate-to-panel) ----------------------------
// 16 kHz mono PCM16, one IMA-ADPCM 4-bit block (241 bytes = 480 samples after
// the header nibble) per call. Returns bytes written (0 = none this tick / no
// mic). Boards without a mic path return 0.
size_t audio_in_hal_read_adpcm(uint8_t* out, size_t cap);
