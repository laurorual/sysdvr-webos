#include "sysdvr/protocol.h"
#include "sysdvr/net.h"
#include "sysdvr/log.h"

#include <stdio.h>
#include <string.h>

#define HANDSHAKE_OK 6u

static uint32_t read_le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    uint64_t lo = (uint64_t)read_le32(p);
    uint64_t hi = (uint64_t)read_le32(p + 4);
    return lo | (hi << 32);
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
    p[2] = (uint8_t)((value >> 16) & 0xFFu);
    p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static int protocol_supported(char a, char b)
{
    return a == '0' && (b == '2' || b == '3');
}

int sysdvr_handshake(
    int fd,
    enum sysdvr_stream_kind stream,
    uint8_t audio_batching,
    char protocol_version_out[3])
{
    uint8_t hello[SYSDVR_HELLO_SIZE];
    uint8_t request[SYSDVR_HANDSHAKE_REQUEST_SIZE];
    uint8_t response[72];

    if (audio_batching > SYSDVR_MAX_AUDIO_BATCHING) {
        LOGF( "[proto] invalid audio batching value: %u\n",
                (unsigned)audio_batching);
        return -1;
    }

    if (sysdvr_read_exact(fd, hello, sizeof(hello)) != (ssize_t)sizeof(hello)) {
        LOGF( "[proto] failed to read 10-byte hello packet\n");
        return -1;
    }

    if (memcmp(hello, "SysDVR|", 7) != 0 || hello[9] != '\0') {
        LOGF( "[proto] invalid SysDVR hello packet\n");
        return -1;
    }

    char ver0 = (char)hello[7];
    char ver1 = (char)hello[8];

    if (!protocol_supported(ver0, ver1)) {
        LOGF( "[proto] unsupported SysDVR protocol: %c%c\n", ver0, ver1);
        return -1;
    }

    protocol_version_out[0] = ver0;
    protocol_version_out[1] = ver1;
    protocol_version_out[2] = '\0';

    memset(request, 0, sizeof(request));
    write_le32(request + 0, SYSDVR_REQUEST_MAGIC);

    /*
     * ProtoVer is a uint16_t in the official structure, but SysDVR compares
     * the two bytes as ASCII in little-endian memory order. Writing the bytes
     * explicitly avoids host packing/endian surprises.
     */
    request[4] = (uint8_t)ver0;
    request[5] = (uint8_t)ver1;

    if (stream == SYSDVR_STREAM_VIDEO) {
        request[6] = 1u << 0; /* ProtoMeta_Video */
        request[7] = 1u << 1; /* ProtoMetaVideo_InjectPPSSPS */
    } else if (stream == SYSDVR_STREAM_AUDIO) {
        request[6] = 1u << 1; /* ProtoMeta_Audio */
    } else {
        LOGF( "[proto] invalid stream kind\n");
        return -1;
    }

    request[8] = audio_batching;
    request[9] = 0; /* FeatureFlags */

    if (sysdvr_write_all(fd, request, sizeof(request)) != (ssize_t)sizeof(request)) {
        LOGF( "[proto] failed to send handshake request\n");
        return -1;
    }

    /*
     * The current official client accepts protocol 02 and 03.
     * v02 returns a 4-byte result. v03 returns the 72-byte response with
     * optional memory-pool diagnostics.
     */
    size_t response_size = (ver1 >= '3') ? 72u : 4u;

    if (sysdvr_read_exact(fd, response, response_size) != (ssize_t)response_size) {
        LOGF( "[proto] failed to read handshake response\n");
        return -1;
    }

    uint32_t result = read_le32(response);
    if (result != HANDSHAKE_OK) {
        LOGF( "[proto] console rejected handshake, code=%u\n",
                (unsigned)result);
        return -1;
    }

    return 0;
}

int sysdvr_read_packet(
    int fd,
    struct sysdvr_packet_header *header,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_size)
{
    uint8_t raw[SYSDVR_PACKET_HEADER_SIZE];

    ssize_t got = sysdvr_read_exact(fd, raw, sizeof(raw));
    if (got == 0) {
        return 0;
    }
    if (got != (ssize_t)sizeof(raw)) {
        LOGF( "[proto] short/failed packet header read\n");
        return -1;
    }

    header->magic = read_le32(raw + 0);
    header->data_size = read_le32(raw + 4);
    header->timestamp_us = read_le64(raw + 8);
    header->metadata = raw[16];
    header->replay_slot = raw[17];

    if (header->magic != SYSDVR_PACKET_MAGIC) {
        LOGF( "[proto] packet magic mismatch: 0x%08x\n",
                (unsigned)header->magic);
        return -1;
    }

    if ((size_t)header->data_size > payload_capacity ||
        header->data_size > SYSDVR_MAX_PAYLOAD) {
        LOGF( "[proto] invalid payload size: %u\n",
                (unsigned)header->data_size);
        return -1;
    }

    /*
     * Replay packets contain no payload. The initial MVP deliberately
     * disables NAL hashing, so receiving one means the stream state is not
     * what we negotiated.
     */
    if ((header->metadata & SYSDVR_PACKET_REPLAY) != 0u) {
        *payload_size = 0;
        LOGF( "[proto] unexpected replay packet (slot %u)\n",
                (unsigned)header->replay_slot);
        return -1;
    }

    if (header->data_size > 0u) {
        if (sysdvr_read_exact(fd, payload, header->data_size) !=
            (ssize_t)header->data_size) {
            LOGF( "[proto] failed to read packet payload\n");
            return -1;
        }
    }

    *payload_size = (size_t)header->data_size;
    return 1;
}

int sysdvr_parse_error_packet(
    const uint8_t *payload,
    size_t payload_size,
    struct sysdvr_error_packet *out)
{
    if (payload_size < 32u || out == NULL) {
        return -1;
    }

    out->error_type = read_le32(payload + 0);
    out->error_code = read_le32(payload + 4);
    out->context1 = read_le64(payload + 8);
    out->context2 = read_le64(payload + 16);
    out->context3 = read_le64(payload + 24);

    return 0;
}
