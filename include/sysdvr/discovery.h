#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sysdvr_discovery sysdvr_discovery;

typedef struct sysdvr_device {
    char host[64];
    char client_version[32];
    char protocol[8];
    char serial[96];
} sysdvr_device;

/* Bind UDP 19999 and prepare a non-blocking SysDVR discovery listener. */
sysdvr_discovery *sysdvr_discovery_open(void);

/*
 * Poll once for a discovery datagram.
 *
 * timeout_ms:
 *   0  = non-blocking
 *   >0 = wait at most this many milliseconds
 *
 * Returns:
 *   1 = valid SysDVR device in `out`
 *   0 = timeout / no valid packet
 *  -1 = socket error
 */
int sysdvr_discovery_poll(
    sysdvr_discovery *scanner,
    int timeout_ms,
    sysdvr_device *out
);

void sysdvr_discovery_close(sysdvr_discovery *scanner);

#ifdef __cplusplus
}
#endif
