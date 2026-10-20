#include "sft/protocol.h"
#include "sysdvr/log.h"
#include "sysdvr/net.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int sft_parse_header(const uint8_t raw[SFT_HEADER_SIZE], sft_packet_header *out)
{
    if (raw == NULL || out == NULL) return -1;
    out->magic = le32(raw + 0);
    out->version = le16(raw + 4);
    out->flags = le16(raw + 6);
    out->width = le32(raw + 8);
    out->height = le32(raw + 12);
    out->console_age_us = le32(raw + 16);
    out->length = le32(raw + 20);
    out->block_height_log2 = le32(raw + 24);
    out->sequence = le32(raw + 28);
    return 0;
}

bool sft_packet_is_status(const sft_packet_header *h)
{
    return h != NULL && (h->flags & SFT_FLAG_STATUS) != 0u;
}

bool sft_packet_is_audio(const sft_packet_header *h)
{
    return h != NULL &&
           (h->flags & SFT_FLAG_AUDIO) != 0u &&
           (h->flags & SFT_FLAG_STATUS) == 0u;
}

bool sft_packet_is_video(const sft_packet_header *h)
{
    return h != NULL &&
           (h->flags & SFT_FLAG_VIDEO) != 0u &&
           (h->flags & (SFT_FLAG_AUDIO | SFT_FLAG_STATUS)) == 0u;
}

int sft_validate_header(const sft_packet_header *h)
{
    if (h == NULL) return -1;
    if (h->magic != SFT_MAGIC || h->version != SFT_PROTOCOL_VERSION) return -1;
    if ((h->flags & ~(SFT_FLAG_VIDEO | SFT_FLAG_KEY | SFT_FLAG_AUDIO | SFT_FLAG_STATUS)) != 0u) return -1;

    if (sft_packet_is_status(h)) {
        if (h->flags != (SFT_FLAG_AUDIO | SFT_FLAG_STATUS) ||
            h->length < 8u || h->length > SFT_MAX_STATUS) return -1;
        return 0;
    }

    if (sft_packet_is_audio(h)) {
        if (h->flags != SFT_FLAG_AUDIO ||
            h->width != 48000u || h->height != 2u ||
            h->length == 0u || h->length > SFT_MAX_AUDIO ||
            (h->length % 4u) != 0u) return -1;
        return 0;
    }

    if (sft_packet_is_video(h)) {
        if (h->width < 16u || h->width > 1920u ||
            h->height < 16u || h->height > 1088u ||
            (h->width & 1u) != 0u || (h->height & 1u) != 0u ||
            h->length == 0u || h->length > SFT_MAX_VIDEO) return -1;
        return 0;
    }

    /* Current network streams should contain video, audio or status. Empty
     * USB keepalive headers are intentionally not accepted on TCP. */
    return -1;
}

static int wait_readable(int fd, int timeout_ms)
{
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int rc = select(fd + 1, &rs, NULL, NULL, &tv);
    if (rc < 0 && errno == EINTR) return 0;
    return rc;
}

static ssize_t read_exact_interruptible(
    int fd,
    atomic_bool *running,
    void *buffer,
    size_t size,
    bool idle_allowed)
{
    uint8_t *p = (uint8_t *)buffer;
    size_t done = 0;
    uint64_t progress_deadline_us = 0;

    while (done < size) {
        if (running != NULL && !atomic_load(running)) return 0;

        if (!idle_allowed || done > 0) {
            if (progress_deadline_us == 0) {
                progress_deadline_us = monotonic_us() + 3000000ULL;
            }
            if (monotonic_us() >= progress_deadline_us) {
                LOGF("[sft-net] stalled read timeout got=%zu expected=%zu idle_for=3.0s\n",
                     done, size);
                return -1;
            }
        }

        int ready = wait_readable(fd, 200);
        if (ready == 0) continue;
        if (ready < 0) {
            LOGF("[sft-net] select failed errno=%d (%s) got=%zu expected=%zu\n",
                 errno, strerror(errno), done, size);
            return -1;
        }

        ssize_t n = recv(fd, p + done, size - done, 0);
        if (n == 0) {
            LOGF("[sft-net] peer closed stream got=%zu expected=%zu\n", done, size);
            return done == 0 ? 0 : -1;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOGF("[sft-net] recv failed errno=%d (%s) got=%zu expected=%zu\n",
                 errno, strerror(errno), done, size);
            return -1;
        }
        done += (size_t)n;
        /* Timeout means no forward progress, not merely a slow large packet.
         * Every successful recv extends the deadline, so a congested stream
         * can keep flowing while a truly wedged sender reconnects quickly. */
        progress_deadline_us = monotonic_us() + 3000000ULL;
    }

    return (ssize_t)done;
}

int sft_read_packet(
    int fd,
    atomic_bool *running,
    sft_packet_header *header,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_size)
{
    if (header == NULL || payload == NULL || payload_size == NULL) return -1;

    uint8_t raw[SFT_HEADER_SIZE];
    uint64_t header_begin = monotonic_us();
    ssize_t got = read_exact_interruptible(fd, running, raw, sizeof(raw), true);
    uint64_t header_end = monotonic_us();
    if (got <= 0) return (int)got;

    if (sft_parse_header(raw, header) != 0 || sft_validate_header(header) != 0) {
        LOGF("[sft-proto] invalid header magic=0x%08x version=%u flags=0x%04x "
             "w=%u h=%u len=%u seq=%u\n",
             header->magic, (unsigned)header->version, (unsigned)header->flags,
             header->width, header->height, header->length, header->sequence);
        return -1;
    }

    if ((size_t)header->length > payload_capacity) {
        LOGF("[sft-proto] payload too large len=%u capacity=%zu\n",
             header->length, payload_capacity);
        return -1;
    }

    uint64_t payload_begin = monotonic_us();
    got = read_exact_interruptible(fd, running, payload, header->length, false);
    uint64_t payload_end = monotonic_us();
    if (got != (ssize_t)header->length) {
        LOGF("[sft-proto] payload read failed seq=%u got=%zd expected=%u\n",
             header->sequence, got, header->length);
        return -1;
    }

    uint64_t header_us = header_end >= header_begin ? header_end - header_begin : 0;
    uint64_t payload_us = payload_end >= payload_begin ? payload_end - payload_begin : 0;
    if (payload_us >= 50000ULL) {
        LOGF("[transport] slow payload read seq=%u bytes=%u duration=%.1fms "
             "header_wait=%.1fms flags=0x%04x\n",
             header->sequence, header->length, (double)payload_us / 1000.0,
             (double)header_us / 1000.0, (unsigned)header->flags);
        sysdvr_log_transport_metrics(fd, "slow-payload");
    }

    *payload_size = (size_t)header->length;
    return 1;
}

static size_t start_code_len(const uint8_t *p, size_t remaining)
{
    if (remaining >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 3;
    if (remaining >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 4;
    return 0;
}

bool sft_packet_is_key(const sft_packet_header *h, const uint8_t *payload, size_t size)
{
    if (h == NULL || !sft_packet_is_video(h)) return false;
    if ((h->flags & SFT_FLAG_KEY) != 0u) return true;

    /* Defensive fallback: recognize an Annex-B IDR even if the explicit KEY
     * bit is absent. */
    for (size_t i = 0; i + 4 < size; ++i) {
        size_t prefix = start_code_len(payload + i, size - i);
        if (prefix == 0 || i + prefix >= size) continue;
        if ((payload[i + prefix] & 0x1fu) == 5u) return true;
        i += prefix - 1u;
    }
    return false;
}

int sft_request_keyframe(int fd)
{
    static const char request[4] = {'S', 'F', 'T', 'K'};
    ssize_t n = sysdvr_write_all(fd, request, sizeof(request));
    if (n != (ssize_t)sizeof(request)) {
        LOGF("[sft-proto] keyframe request failed written=%zd\n", n);
        return -1;
    }
    LOGF("[sft-proto] requested fresh keyframe\n");
    return 0;
}
