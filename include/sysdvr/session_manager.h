#pragma once

#include "sysdvr/discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYSDVR_SESSION_MAX_DEVICES 8
#define SYSDVR_DEVICE_STALE_MS 7000ULL

typedef struct session_device {
    sysdvr_device info;
    uint64_t last_seen_ms;
    bool manual;
} session_device;

typedef struct session_manager {
    session_device devices[SYSDVR_SESSION_MAX_DEVICES];
    size_t count;
    size_t selected;

    char preferred_serial[96];
    char preferred_host[64];
    bool have_preferred;
} session_manager;

void session_manager_init(session_manager *manager);

void session_manager_seen(
    session_manager *manager,
    const sysdvr_device *device,
    uint64_t now_ms
);

void session_manager_add_manual(
    session_manager *manager,
    const char *host
);

void session_manager_prune(
    session_manager *manager,
    uint64_t now_ms
);

void session_manager_move(
    session_manager *manager,
    int delta
);

const session_device *session_manager_selected(
    const session_manager *manager
);

void session_manager_remember_selected(
    session_manager *manager
);

bool session_manager_matches_preferred(
    const session_manager *manager,
    const sysdvr_device *device
);

#ifdef __cplusplus
}
#endif
