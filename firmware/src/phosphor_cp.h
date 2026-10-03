#pragma once
// Phosphor (thin) glyph codepoints — codepoint-checked against
// phosphor-icons-core d42782b/src/icons.ts (18/18 verified 2026-10-03).
// Rendered by font_phos_40.c / font_phos_28.c / font_phos_20.c.
// UTF-8 of U+E0xx..U+E8xx plane is 3 bytes: 0xEE + ((cp>>6)&0x3F with 0x80
// bias) + continuation — written here as explicit escape strings.
#define PH_CLOCK          "\xEE\x86\x9A"   // U+E19A  clock
#define PH_BATTERY_CHARGE "\xEE\x82\xBA"   // U+E0BA  battery-charging
#define PH_BATTERY_EMPTY  "\xEE\x82\xBE"   // U+E0BE  battery-empty
#define PH_BATTERY_FULL   "\xEE\x83\x80"   // U+E0C0  battery-full
#define PH_BATTERY_HIGH   "\xEE\x83\x82"   // U+E0C2  battery-high
#define PH_BATTERY_LOW    "\xEE\x83\x84"   // U+E0C4  battery-low
#define PH_BATTERY_MED    "\xEE\x83\x86"   // U+E0C6  battery-medium
#define PH_WIFI           "\xEE\x93\xAA"   // U+E4EA  wifi-high
#define PH_BLUETOOTH      "\xEE\x83\x9A"   // U+E0DA  bluetooth
#define PH_LIST           "\xEE\x8B\xB0"   // U+E2F0  list
#define PH_GAUGE          "\xEE\x98\xA8"   // U+E628  gauge
#define PH_SLIDERS        "\xEE\x90\xB4"   // U+E434  sliders-horizontal
#define PH_GEAR           "\xEE\x89\xB0"   // U+E270  gear
#define PH_MAP_PIN        "\xEE\x8C\x96"   // U+E316  map-pin
#define PH_SPEAKER_HIGH   "\xEE\x91\x8A"   // U+E44A  speaker-high
#define PH_WAVEFORM       "\xEE\xA0\x82"   // U+E802  waveform
#define PH_MICROPHONE     "\xEE\x8C\xA6"   // U+E326  microphone
#define PH_GLOBE          "\xEE\x8A\x88"   // U+E288  globe