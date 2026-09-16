#pragma once
#include <stdint.h>
#include <stdbool.h>

// Host bridge — Claude Code session list + resume via the BLE daemon on macOS.

struct HostSessionEntry {
    char id[37];
    char title[41];
    char project[25];
};

#define HOST_BRIDGE_MAX_SESSIONS 10

typedef enum {
    HOST_BRIDGE_IDLE = 0,
    HOST_BRIDGE_LOADING,
    HOST_BRIDGE_LIST_READY,
    HOST_BRIDGE_RESUME_SENT,
    HOST_BRIDGE_ERROR,
} host_bridge_state_t;

void            host_bridge_reset(void);
void            host_bridge_request_list(void);
void            host_bridge_resume(const char* session_id);
host_bridge_state_t host_bridge_state(void);
int             host_bridge_session_count(void);
const HostSessionEntry* host_bridge_sessions(void);
const char*     host_bridge_message(void);
void            host_bridge_tick(void);
void            host_bridge_on_rx(const char* json);
