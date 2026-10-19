#include "apps/voice/voice_layout.h"

#include <cstdio>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void check_inside(const VoiceRect& rect, int width, int height) {
    CHECK(rect.x >= 0);
    CHECK(rect.y >= 0);
    CHECK(rect.w > 0);
    CHECK(rect.h > 0);
    CHECK(rect.x + rect.w <= width);
    CHECK(rect.y + rect.h <= height);
}

static void check_layout(int width, int height, int minimum_transcript_height) {
    constexpr int statusbar_h = 40;
    constexpr int foot_h = 34;
    const VoiceLayout layout = voice_layout(width, height, statusbar_h, foot_h);

    check_inside(layout.mic, width, height);
    check_inside(layout.state, width, height);
    check_inside(layout.meter, width, height);
    check_inside(layout.transcript, width, height);

    CHECK(layout.mic.w == layout.mic.h);
    CHECK(layout.mic.y >= statusbar_h);
    CHECK(layout.mic.y + layout.mic.h <= layout.state.y);
    CHECK(layout.state.y + layout.state.h <= layout.meter.y);
    CHECK(layout.meter.y + layout.meter.h <= layout.transcript.y);
    CHECK(layout.transcript.y + layout.transcript.h <= height - foot_h);
    CHECK(layout.transcript.h >= minimum_transcript_height);
}

static void reference_layout_keeps_three_lines_clear() {
    check_layout(480, 480, 66);
}

static void compact_layout_keeps_copy_out_of_controls() {
    check_layout(368, 448, 54);
}

int main() {
    reference_layout_keeps_three_lines_clear();
    compact_layout_keeps_copy_out_of_controls();

    if (failures) {
        std::printf("%d voice layout check(s) failed\n", failures);
        return 1;
    }
    std::printf("all voice layout checks passed\n");
    return 0;
}
