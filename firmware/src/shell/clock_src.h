#pragma once
#include <stdbool.h>
#include <time.h>

// Shared wall-clock source.
//
// The board has no network time of its own in the default configuration, so
// the host supplies local wall-clock seconds with each usage payload (~60s
// apart) and we advance it locally from lv_tick between beats. The status bar
// and the Clock app both read from here so they can never disagree.
//
// "Local wall-clock seconds" means the host already applied its timezone —
// treat the value as UTC when formatting (gmtime) to render it unchanged.

void clock_src_set(long epoch_local, int fmt);  // fmt: 12 or 24
bool clock_src_valid(void);
int  clock_src_fmt(void);

// Fills `out` with the current time. Returns false (leaving `out` untouched)
// if the host hasn't sent a time yet.
bool clock_src_now(struct tm* out);

// Formats the current time as "14:05" or "2:05 PM". Writes "--:--" when no
// time is known, so callers can render unconditionally.
void clock_src_format(char* buf, size_t len);

// Like clock_src_format but includes seconds. Writes "--:--:--" when unknown.
void clock_src_format_hms(char* buf, size_t len);

// "Mon, Sep 14" from the synced clock. Writes a waiting hint when unknown.
void clock_src_format_date(char* buf, size_t len);
