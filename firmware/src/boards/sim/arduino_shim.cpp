#include <Arduino.h>
#include <Preferences.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

unsigned long millis(void) {
    static uint64_t t0 = now_ms();
    return (unsigned long)(now_ms() - t0);
}

void delay(unsigned long ms) { usleep(ms * 1000); }

size_t SimSerial::write(const uint8_t* buf, size_t len) {
    return fwrite(buf, 1, len, stdout);
}

// ---- Serial input: non-blocking stdin ----
// Drained into a small ring so available()/read() can never block the render
// loop. Overflow drops the oldest byte: this path only ever carries short
// hand- or script-typed commands, so a full buffer means nobody is reading.
#define SERIAL_IN_CAP 256
static char   in_buf[SERIAL_IN_CAP];
static size_t in_head = 0, in_tail = 0;

static void pump_stdin(void) {
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {0, 0};   // poll, never wait
        if (select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv) <= 0) return;

        char c;
        if (::read(STDIN_FILENO, &c, 1) != 1) return;   // EOF or error
        size_t next = (in_head + 1) % SERIAL_IN_CAP;
        if (next == in_tail) in_tail = (in_tail + 1) % SERIAL_IN_CAP;
        in_buf[in_head] = c;
        in_head = next;
    }
}

int SimSerial::available(void) {
    pump_stdin();
    return (int)((in_head + SERIAL_IN_CAP - in_tail) % SERIAL_IN_CAP);
}

int SimSerial::read(void) {
    pump_stdin();
    if (in_head == in_tail) return -1;
    char c = in_buf[in_tail];
    in_tail = (in_tail + 1) % SERIAL_IN_CAP;
    return (unsigned char)c;
}
void SimSerial::printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
SimSerial Serial;

// ---- Preferences: tiny in-memory key/value store ----
#define PREF_SLOTS 16
static struct { char key[16]; uint8_t val; bool used; } store[PREF_SLOTS];

uint8_t Preferences::getUChar(const char* key, uint8_t def) {
    for (auto& s : store)
        if (s.used && strcmp(s.key, key) == 0) return s.val;
    return def;
}
size_t Preferences::putUChar(const char* key, uint8_t value) {
    for (auto& s : store)
        if (s.used && strcmp(s.key, key) == 0) { s.val = value; return 1; }
    for (auto& s : store)
        if (!s.used) {
            s.used = true;
            strncpy(s.key, key, sizeof(s.key) - 1);
            s.val = value;
            return 1;
        }
    return 0;
}
