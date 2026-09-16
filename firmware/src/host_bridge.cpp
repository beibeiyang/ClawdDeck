#include "host_bridge.h"
#include "ble.h"
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

static host_bridge_state_t state = HOST_BRIDGE_IDLE;
static HostSessionEntry    entries[HOST_BRIDGE_MAX_SESSIONS];
static int                 entry_count = 0;
static char                message[64] = "";

void host_bridge_reset(void) {
    state       = HOST_BRIDGE_IDLE;
    entry_count = 0;
    message[0]  = '\0';
}

void host_bridge_request_list(void) {
    entry_count = 0;
    message[0]  = '\0';
    state       = HOST_BRIDGE_LOADING;
    ble_bridge_send_cmd("{\"c\":\"ls\"}");
}

void host_bridge_resume(const char* session_id) {
    if (!session_id || !session_id[0]) return;
    message[0] = '\0';
    state      = HOST_BRIDGE_RESUME_SENT;
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"c\":\"rs\",\"i\":\"%s\"}", session_id);
    ble_bridge_send_cmd(buf);
}

host_bridge_state_t host_bridge_state(void) { return state; }
int host_bridge_session_count(void) { return entry_count; }
const HostSessionEntry* host_bridge_sessions(void) { return entries; }
const char* host_bridge_message(void) { return message; }

void host_bridge_on_rx(const char* json) {
    if (!json || !json[0]) return;

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        state = HOST_BRIDGE_ERROR;
        snprintf(message, sizeof(message), "Bad bridge payload");
        return;
    }

    const char* typ = doc["t"] | "";
    if (strcmp(typ, "ss") == 0) {
        entry_count = 0;
        JsonArray arr = doc["n"].as<JsonArray>();
        for (JsonObject item : arr) {
            if (entry_count >= HOST_BRIDGE_MAX_SESSIONS) break;
            HostSessionEntry* e = &entries[entry_count++];
            const char* id = item["i"] | "";
            const char* title = item["l"] | "";
            const char* proj = item["p"] | "";
            strncpy(e->id, id, sizeof(e->id) - 1);
            strncpy(e->title, title, sizeof(e->title) - 1);
            strncpy(e->project, proj, sizeof(e->project) - 1);
        }
        state = HOST_BRIDGE_LIST_READY;
        message[0] = '\0';
        return;
    }

    if (strcmp(typ, "ok") == 0) {
        const char* msg = doc["m"] | "Done";
        snprintf(message, sizeof(message), "%s", msg);
        state = HOST_BRIDGE_IDLE;
        return;
    }

    if (strcmp(typ, "err") == 0) {
        const char* msg = doc["m"] | "Host error";
        snprintf(message, sizeof(message), "%s", msg);
        state = HOST_BRIDGE_ERROR;
    }
}

void host_bridge_tick(void) {
    const char* json = ble_bridge_take_rx();
    if (json) host_bridge_on_rx(json);
}
