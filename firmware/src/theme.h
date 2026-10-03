#pragma once
#include <lvgl.h>

// Design tokens — single source of truth for UI colors. Anthropic-inspired
// dark palette, AMOLED-friendly, tuned for the Liquid Glass look: translucent
// panels over a dark wallpaper, bright top rims, deep bottoms (see
// shell/launcher.cpp glass_tile / statusbar glass pill).
//
// ---- Liquid Glass helpers -------------------------------------------------
// Every glass panel shares one recipe: a dark translucent fill whose bottom is
// denser than its top (the "depth" gradient: light passes the thin top of the
// material and pools at its base), plus a top-edge specular highlight.
// SHEEN_TINT is a hint of the wallpaper color, so panels read as glass instead
// of grey plastic. PANEL_OPA is the baseline fill; the same panel type can
// override depth via bg_grad_dir + BRIGHT/HI/SHADE stops.
#define GLASS_PANEL_OPA    140      // base translucent fill (0-255)
#define GLASS_HI_OPA       48       // upper, more translucent stop
#define GLASS_SHADE_OPA    186      // lower, denser stop
//
// Wallpaper — warm-to-cool duotone flow (Apple Waveform wallpaper family):
// warm taupe light pools top-left, drifts down through plum-mid to a deep
// navy that stays near-black for AMOLED battery. The glass panels need this
// light to sheen, tint-pool and rim-glint OFF of — dead black reads as flat
// plastic. Stops are ≤ +0x11 luma apart to keep RGB565 from banding.
#define THEME_BG       lv_color_hex(0x0a0d16)                          // deep navy base
#define THEME_BG_TOP   lv_color_hex(0x4e3d36)                          // warm taupe light
#define THEME_BG_MID   lv_color_hex(0x241d24)                          // plum transition
#define THEME_BG_BOT   lv_color_hex(0x0a0d16)                          // navy floor
#define SHEEN_TINT     lv_color_hex(0xe9d8c9)                          // warm cream (wallpaper light through glass)
//
// ---- Ink / chrome ----------------------------------------------------------
#define THEME_TEXT     lv_color_hex(0xfaf9f5)   // primary text (over dark glass)
#define THEME_INK      lv_color_hex(0x231a12)   // dark ink on light glass
#define THEME_DIM      lv_color_hex(0xb0aea5)   // secondary text
#define THEME_ACCENT   lv_color_hex(0xd97757)   // brand terra-cotta
#define THEME_GREEN    lv_color_hex(0x788c5d)
#define THEME_AMBER    lv_color_hex(0xd97757)
#define THEME_RED      lv_color_hex(0xc0392b)
#define THEME_BAR_BG   lv_color_hex(0x2a2a28)   // unfilled bar track
//
// ---- Glass chrome (rims / shadows / highlights) ----
#define RIM_HI         lv_color_hex(0xffffff)   // 1px specular top rim
#define RIM_CREAM      lv_color_hex(0xf2e6da)   // warm rim over the light pool
#define RIM_EDGE       lv_color_hex(0x5a6072)   // mid-grey rim stroke
#define RIM_DARK       lv_color_hex(0x0a0c12)   // outer contour
#define GLASS_SHADOW   lv_color_hex(0x000000)   // drop shadow behind glass
#define GLASS_HIGHLIGHT_OPA 46                  // press-highlight alpha
//
// ---- Legacy alias kept for other screens (ui.cpp usage panels still use it) ----
#define THEME_PANEL    lv_color_hex(0x1f1f1e)   // card/zone fill