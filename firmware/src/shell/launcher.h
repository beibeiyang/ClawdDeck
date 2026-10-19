#pragma once
#include <lvgl.h>

// Home screen: a paged grid of app tiles, iOS-style. Built once at boot from
// the shell's capability-filtered app list and then shown/hidden — the tiles
// never change at runtime.

void launcher_init(lv_obj_t* parent);
void launcher_set_visible(bool visible);

// Jump straight to a page (0-based, clamped). Lets a screenshot of any page be
// scripted over serial, since neither the sim nor a headless capture can swipe.
void launcher_goto_page(int page);
int  launcher_page_count(void);

// ---- Hardware swipe engine (CO5300 + factory-look parity) -----------------
// The live-widget page swipe re-renders ~195k px per frame (measured on the
// 2.16: 100+ ms/frame = 10 FPS). The factory-smooth approach slides the page
// CONTENT as pre-baked bitmaps: LVGL scrolls baked images (one blit per frame
// band), and the swipe driver paints a full-width GRAM band directly from the
// page bitmaps when the finger is down. While a swipe is active LVGL's own
// indev-driven scrolling is suppressed; on release the engine animates the
// settle, hands control back to LVGL, and re-enables it.
void launcher_swipe_press(int x, int y);
void launcher_swipe_move(int x, int y);
void launcher_swipe_release(void);