#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SFT_MAGIC 0x52544653u
#define SFT_PROTOCOL_VERSION 2u
#define SFT_HEADER_SIZE 32u
#define SFT_MAX_VIDEO (8u * 1024u * 1024u)
#define SFT_MAX_AUDIO 65536u
#define SFT_MAX_STATUS 4096u
#define SFT_MAX_PAYLOAD SFT_MAX_VIDEO

#define SFT_FLAG_VIDEO  0x02u
#define SFT_FLAG_KEY    0x04u
#define SFT_FLAG_AUDIO  0x08u
#define SFT_FLAG_STATUS 0x10u

typedef struct sft_packet_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t console_age_us;
    uint32_t length;
    uint32_t block_height_log2;
    uint32_t sequence;
} sft_packet_header;

/* Parse a 32-byte SFTR v2 header. Returns 0 on success. */
int sft_parse_header(
    const uint8_t raw[SFT_HEADER_SIZE],
    sft_packet_header *out
);

/* Validate header semantics and payload bounds. Returns 0 on success. */
int sft_validate_header(const sft_packet_header *header);

/*
 * Read one packet while periodically checking `running`.
 * Idle between headers may last indefinitely. Once a header/payload starts,
 * the read fails only after 3 seconds without forward progress. Successful
 * partial reads extend the deadline so large congested frames can still finish.
 *
 * Returns:
 *   1 packet read
 *   0 clean EOF / stopped
 *  -1 malformed packet / transport error
 */
int sft_read_packet(
    int fd,
    atomic_bool *running,
    sft_packet_header *header,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_size
);

bool sft_packet_is_video(const sft_packet_header *header);
bool sft_packet_is_audio(const sft_packet_header *header);
bool sft_packet_is_status(const sft_packet_header *header);
bool sft_packet_is_key(const sft_packet_header *header, const uint8_t *payload, size_t size);

/* Ask current Switch Frame Tap releases for a fresh keyframe. */
int sft_request_keyframe(int fd);

#ifdef __cplusplus
}
#endif
