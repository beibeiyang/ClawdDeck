#pragma once
#include <stdint.h>

enum ble_state_t {
    BLE_STATE_INIT,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
};

void ble_init(void);
void ble_tick(void);
ble_state_t ble_get_state(void);
const char* ble_get_device_name(void);
const char* ble_get_mac_address(void);
void ble_clear_bonds(void);
void ble_set_pairable(bool on);   // air-gap gate: false rejects all new bonds + stops advertising
bool ble_is_pairable(void);
bool ble_has_bonds(void);
bool ble_has_data(void);
const char* ble_get_data(void);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_request_refresh(void);

// Host bridge (Sessions app): device notifies commands, host writes responses.
void ble_bridge_send_cmd(const char* json);
const char* ble_bridge_take_rx(void);

// Voice dictation: binary chunk stream device -> host (VOICE char 0x07).
// One call = one notification; seq/flags/len envelope, payload ≤ 244 bytes.
// flags bit0 = end-of-utterance.
void ble_voice_send_chunk(uint8_t seq, uint8_t flags, const uint8_t* data, uint8_t len);

void ble_set_battery_level(int pct);

// BLE HID keyboard
void ble_keyboard_press(uint8_t key, uint8_t modifier);
void ble_keyboard_release(void);
