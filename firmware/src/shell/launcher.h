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
