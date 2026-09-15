#pragma once
#include <stdint.h>

// Runtime board description consumed by board-agnostic code (UI, main loop).
// Each board provides a single BoardCaps instance via board_caps().
//
// Compile-time-only facts (pin numbers, library choice) belong in
// boards/<name>/board.h and never leak into shared code. Anything the UI or
// main loop needs at runtime — display size, optional-feature presence —
// goes here so shared code stays free of #ifdef BOARD_*.
struct BoardCaps {
    const char* name;        // human-readable, e.g. "Waveshare AMOLED 2.16"

    int16_t width;           // active display width in pixels
    int16_t height;          // active display height in pixels

    uint8_t button_count;    // 1 = primary (BOOT) only; 2 = primary + secondary
    bool    has_rotation;    // IMU-driven CPU rotation in the flush callback
    bool    has_battery;     // AXP2101 battery measurement is meaningful
    bool    has_imu;         // QMI8658 (or compatible) is populated

    // Added for the app launcher: it hides tiles whose hardware this board
    // lacks, so an app that needs a mic simply never appears on a mic-less
    // board. Default false — a port opts in only once the path is verified on
    // real hardware, which is why boards whose codec is wired but untested
    // (2.06, 1.8-C6) deliberately leave has_sound off.
    bool    has_sound;       // speaker playback path works (chime engine)
    bool    has_mic;         // audio_in capture works (ES7210 on the 2.16)
    bool    has_wifi;        // station mode is available and verified
};

const BoardCaps& board_caps(void);
