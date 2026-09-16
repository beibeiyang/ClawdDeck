#include "clock_src.h"
#include <lvgl.h>
#include <stdio.h>

static long     base_epoch = 0;   // 0 = never set
static uint32_t base_ms    = 0;
static int      s_fmt      = 24;

void clock_src_set(long epoch_local, int fmt) {
    if (epoch_local <= 0) {   // host turned the clock off
        base_epoch = 0;
        return;
    }
    base_epoch = epoch_local;
    base_ms    = lv_tick_get();
    s_fmt      = (fmt == 12) ? 12 : 24;
}

bool clock_src_valid(void) { return base_epoch > 0; }
int  clock_src_fmt(void)   { return s_fmt; }

bool clock_src_now(struct tm* out) {
    if (base_epoch <= 0) return false;
    time_t cur = (time_t)(base_epoch + (lv_tick_get() - base_ms) / 1000);
    gmtime_r(&cur, out);   // already local wall-clock; gmtime keeps it verbatim
    return true;
}

void clock_src_format(char* buf, size_t len) {
    struct tm t;
    if (!clock_src_now(&t)) {
        snprintf(buf, len, "--:--");
        return;
    }
    if (s_fmt == 12) {
        int h12 = t.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, len, "%d:%02d %s", h12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, len, "%02d:%02d", t.tm_hour, t.tm_min);
    }
}

void clock_src_format_hms(char* buf, size_t len) {
    struct tm t;
    if (!clock_src_now(&t)) {
        snprintf(buf, len, "--:--:--");
        return;
    }
    if (s_fmt == 12) {
        int h12 = t.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, len, "%d:%02d:%02d %s", h12, t.tm_min, t.tm_sec,
                 t.tm_hour < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, len, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    }
}

void clock_src_format_date(char* buf, size_t len) {
    struct tm t;
    if (!clock_src_now(&t)) {
        snprintf(buf, len, "Waiting for host time…");
        return;
    }
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(buf, len, "%s, %s %d", days[t.tm_wday], months[t.tm_mon], t.tm_mday);
}
