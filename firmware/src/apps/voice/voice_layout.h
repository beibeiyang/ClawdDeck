#pragma once

struct VoiceRect {
    int x;
    int y;
    int w;
    int h;
};

struct VoiceLayout {
    VoiceRect mic;
    VoiceRect state;
    VoiceRect meter;
    VoiceRect transcript;
};

inline VoiceLayout voice_layout(int width, int height, int statusbar_h,
                                int foot_h) {
    const bool compact = width < 400;
    const int side_margin = compact ? 20 : 28;
    const int transcript_height = compact ? 68 : 78;
    const int bottom = height - foot_h - 10;
    const int transcript_y = bottom - transcript_height;
    const VoiceRect transcript = {
        side_margin, transcript_y, width - side_margin * 2, transcript_height
    };

    const int meter_height = 6;
    const int meter_y = transcript_y - (compact ? 20 : 22);
    const VoiceRect meter = {
        width / 2 - (compact ? 64 : 76), meter_y,
        compact ? 128 : 152, meter_height
    };

    const int state_height = compact ? 24 : 26;
    const int state_y = meter_y - (compact ? 32 : 34);
    const VoiceRect state = {
        side_margin, state_y, width - side_margin * 2, state_height
    };

    int mic_size = compact ? 116 : 152;
    const int mic_bottom = state_y - (compact ? 14 : 16);
    const int max_mic_size = mic_bottom - statusbar_h - 20;
    if (mic_size > max_mic_size) mic_size = max_mic_size;
    if (mic_size < 72) mic_size = 72;
    const VoiceRect mic = {
        (width - mic_size) / 2, mic_bottom - mic_size, mic_size, mic_size
    };

    VoiceLayout layout = {mic, state, meter, transcript};
    return layout;
}
