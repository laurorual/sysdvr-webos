#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SFT_STREAM_PORT 9950u
#define SFT_BEACON_PORT 9951u

typedef struct sft_discovery sft_discovery;

typedef struct sft_device {
    char host[64];
    uint16_t port;
    char label[96];
} sft_device;

sft_discovery *sft_discovery_open(void);

/*
 * 1 = valid Switch Frame Tap beacon
 * 0 = timeout / irrelevant datagram
 * -1 = socket error
 */
int sft_discovery_poll(
    sft_discovery *scanner,
    int timeout_ms,
    sft_device *out
);

void sft_discovery_close(sft_discovery *scanner);

#ifdef __cplusplus
}
#endif
