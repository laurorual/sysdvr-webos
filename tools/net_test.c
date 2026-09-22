#include "sysdvr/net.h"
#include "sysdvr/protocol.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void on_signal(int signum)
{
    (void)signum;
    g_running = 0;
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s <switch-ip-or-hostname> [capture.h264]\n"
            "\n"
            "The Switch must have SysDVR configured in TCP Bridge mode and a\n"
            "compatible game must be running.\n",
            argv0);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        return 2;
    }

    const char *host = argv[1];
    const char *output_path = argc == 3 ? argv[2] : "capture.h264";

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "[test] connecting to %s:%u...\n",
            host, (unsigned)SYSDVR_VIDEO_PORT);

    int fd = sysdvr_tcp_connect(host, SYSDVR_VIDEO_PORT);
    if (fd < 0) {
        return 1;
    }

    char protocol[3];
    if (sysdvr_handshake(fd, SYSDVR_STREAM_VIDEO, 0, protocol) != 0) {
        close(fd);
        return 1;
    }

    fprintf(stderr, "[test] handshake OK (protocol %s)\n", protocol);

    FILE *out = fopen(output_path, "wb");
    if (out == NULL) {
        perror("[test] fopen");
        close(fd);
        return 1;
    }

    uint8_t *payload = malloc(SYSDVR_MAX_PAYLOAD);
    if (payload == NULL) {
        fprintf(stderr, "[test] could not allocate packet buffer\n");
        fclose(out);
        close(fd);
        return 1;
    }

    uint64_t packet_count = 0;
    uint64_t byte_count = 0;
    uint64_t last_timestamp = 0;

    fprintf(stderr,
            "[test] receiving H.264 -> %s\n"
            "[test] press Ctrl+C to stop\n",
            output_path);

    int exit_code = 0;

    while (g_running) {
        struct sysdvr_packet_header header;
        size_t payload_size = 0;

        int rc = sysdvr_read_packet(
            fd, &header, payload, SYSDVR_MAX_PAYLOAD, &payload_size);

        if (rc == 0) {
            fprintf(stderr, "\n[test] console closed the stream\n");
            break;
        }

        if (rc < 0) {
            fprintf(stderr, "\n[test] stream read failed\n");
            exit_code = 1;
            break;
        }

        if ((header.metadata & SYSDVR_PACKET_ERROR) != 0u) {
            struct sysdvr_error_packet error;
            if (sysdvr_parse_error_packet(payload, payload_size, &error) == 0) {
                fprintf(stderr,
                        "\n[test] SysDVR error type=%u code=0x%08x "
                        "ctx=%llu/%llu/%llu\n",
                        (unsigned)error.error_type,
                        (unsigned)error.error_code,
                        (unsigned long long)error.context1,
                        (unsigned long long)error.context2,
                        (unsigned long long)error.context3);
            } else {
                fprintf(stderr, "\n[test] malformed SysDVR error packet\n");
            }
            exit_code = 1;
            break;
        }

        if ((header.metadata & SYSDVR_PACKET_VIDEO) == 0u) {
            fprintf(stderr, "\n[test] non-video packet on video socket\n");
            continue;
        }

        if (payload_size > 0u) {
            if (fwrite(payload, 1, payload_size, out) != payload_size) {
                perror("\n[test] fwrite");
                exit_code = 1;
                break;
            }

            packet_count++;
            byte_count += payload_size;
            last_timestamp = header.timestamp_us;
        }

        if ((packet_count % 120u) == 0u && packet_count != 0u) {
            fprintf(stderr,
                    "\r[test] packets=%llu  MiB=%.2f  media=%.2fs",
                    (unsigned long long)packet_count,
                    (double)byte_count / (1024.0 * 1024.0),
                    (double)last_timestamp / 1000000.0);
            fflush(stderr);
        }
    }

    fprintf(stderr,
            "\n[test] done: %llu packets, %.2f MiB\n",
            (unsigned long long)packet_count,
            (double)byte_count / (1024.0 * 1024.0));

    free(payload);
    fclose(out);
    close(fd);

    return exit_code;
}
