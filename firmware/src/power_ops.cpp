#include "hal/power_hal.h"

#ifdef BOARD_SIM
// The sim is a desktop process: "off" = a clean exit, "restart" = exit (the
// harness/daemon relaunches it; there is no bootloader here).
#include <cstdlib>
void __attribute__((weak)) power_hal_power_off(void) { std::exit(0); }
void power_hal_restart(void) { std::exit(0); }
#else
#include <esp_system.h>
#include <esp_sleep.h>

// Weak default: boards whose PMU driver doesn't provide a regulated rail-off
// fall back to deep sleep until reset. A board's power.cpp provides a strong
// override (Waveshare AMOLED-2.16: AXP2101 poweroff — a one-shot true off).
void __attribute__((weak)) power_hal_power_off(void) {
    esp_deep_sleep_start();
}

void power_hal_restart(void) {
    esp_restart();
}
#endif
