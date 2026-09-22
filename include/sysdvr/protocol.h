#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYSDVR_VIDEO_PORT 9911
#define SYSDVR_AUDIO_PORT 9922
#define SYSDVR_DISCOVERY_PORT 19999

#define SYSDVR_HELLO_SIZE 10
#define SYSDVR_HANDSHAKE_REQUEST_SIZE 16
#define SYSDVR_PACKET_HEADER_SIZE 18

#define SYSDVR_REQUEST_MAGIC 0xAAAAAAAAu
#define SYSDVR_PACKET_MAGIC  0xCCCCCCCCu

#define SYSDVR_VIDEO_BUFFER_SIZE 0x54000u
#define SYSDVR_AUDIO_BUFFER_SIZE 0x1000u
#define SYSDVR_MAX_AUDIO_BATCHING 5u /* current sysmodule capture.h */
#define SYSDVR_MAX_PAYLOAD SYSDVR_VIDEO_BUFFER_SIZE

enum sysdvr_stream_kind {
    SYSDVR_STREAM_VIDEO = 1,
    SYSDVR_STREAM_AUDIO = 2
};

enum sysdvr_packet_flags {
    SYSDVR_PACKET_VIDEO     = 1u << 0,
    SYSDVR_PACKET_AUDIO     = 1u << 1,
    SYSDVR_PACKET_DATA      = 1u << 2,
    SYSDVR_PACKET_REPLAY    = 1u << 3,
    SYSDVR_PACKET_MULTI_NAL = 1u << 4,
    SYSDVR_PACKET_ERROR     = 1u << 5
};

struct sysdvr_packet_header {
    uint32_t magic;
    uint32_t data_size;
    uint64_t timestamp_us;
    uint8_t metadata;
    uint8_t replay_slot;
};

struct sysdvr_error_packet {
    uint32_t error_type;
    uint32_t error_code;
    uint64_t context1;
    uint64_t context2;
    uint64_t context3;
};

/*
 * Performs the SysDVR TCP Bridge handshake on an already-connected socket.
 *
 * We intentionally start with:
 *   - NAL replay/hash disabled
 *   - SPS/PPS injection enabled for video
 *   - no extra feature flags
 *
 * protocol_version_out must have room for 3 bytes ("02"/"03" + NUL).
 *
 * Returns 0 on success, -1 on transport/protocol failure.
 */
int sysdvr_handshake(
    int fd,
    enum sysdvr_stream_kind stream,
    uint8_t audio_batching,
    char protocol_version_out[3]
);

/*
 * Reads one full SysDVR stream packet.
 *
 * payload must be at least SYSDVR_MAX_PAYLOAD bytes for general use.
 * payload_size receives the number of bytes read after the header.
 *
 * Returns:
 *   1  packet read successfully
 *   0  clean EOF before another header
 *  -1  malformed packet or transport error
 */
int sysdvr_read_packet(
    int fd,
    struct sysdvr_packet_header *header,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_size
);

int sysdvr_parse_error_packet(
    const uint8_t *payload,
    size_t payload_size,
    struct sysdvr_error_packet *out
);

#ifdef __cplusplus
}
#endif
