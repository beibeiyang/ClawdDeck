#include "apps.h"

// Each app defines its own AppDef; this file is the single place that decides
// which are compiled in and what order they appear on the home screen.
// Placeholders for apps not yet implemented (see apps/<id>/app.cpp when built).
extern const AppDef app_clawdmeter;
extern const AppDef app_voice;
extern const AppDef app_sessions;
extern const AppDef app_clock;
extern const AppDef app_micscope;
extern const AppDef app_level;
extern const AppDef app_hostmon;
extern const AppDef app_settings;

// Launcher order. The grid is a fixed 2x2, so these eight fill exactly two
// pages on every panel, and any app whose hardware is missing is filtered out
// by the shell before the grid is built.
static const AppDef* const table[] = {
    &app_clawdmeter,
    &app_voice,
    &app_sessions,
    &app_clock,
    &app_micscope,
    &app_level,
    &app_hostmon,
    &app_settings,
};

int apps_count(void) { return (int)(sizeof(table) / sizeof(table[0])); }

const AppDef* apps_at(int i) {
    if (i < 0 || i >= apps_count()) return nullptr;
    return table[i];
}
