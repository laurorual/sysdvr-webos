#include "sft/discovery.h"
#include "sysdvr/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

struct sft_discovery {
    int fd;
};

static int parse_port(const char *text, uint16_t *out)
{
    if (text == NULL || *text == '\0') return 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0 || value > 65535) return 0;
    *out = (uint16_t)value;
    return 1;
}

static int parse_beacon(
    const char *message,
    const struct sockaddr_in *remote,
    sft_device *out)
{
    if (message == NULL || remote == NULL || out == NULL) return 0;

    char copy[256];
    snprintf(copy, sizeof(copy), "%s", message);

    char *save = NULL;
    char *magic = strtok_r(copy, " \t\r\n", &save);
    char *announced_ip = strtok_r(NULL, " \t\r\n", &save);
    char *port_text = strtok_r(NULL, " \t\r\n", &save);

    if (magic == NULL || strcmp(magic, "SFTAP1") != 0 ||
        announced_ip == NULL || port_text == NULL) {
        return 0;
    }

    uint16_t port = 0;
    if (!parse_port(port_text, &port)) return 0;

    memset(out, 0, sizeof(*out));
    if (inet_ntop(AF_INET, &remote->sin_addr, out->host, sizeof(out->host)) == NULL) {
        return 0;
    }
    out->port = port;

    /* The sender address is authoritative; the IP inside the beacon can name
     * another interface. Keep the remaining words only as a friendly label. */
    if (save != NULL) {
        while (*save == ' ' || *save == '\t') ++save;
        snprintf(out->label, sizeof(out->label), "%s", *save ? save : "Switch Frame Tap");
        size_t n = strlen(out->label);
        while (n > 0 && (out->label[n-1] == '\r' || out->label[n-1] == '\n')) {
            out->label[--n] = '\0';
        }
    }
    if (out->label[0] == '\0') snprintf(out->label, sizeof(out->label), "%s", "Switch Frame Tap");

    (void)announced_ip;
    return 1;
}

sft_discovery *sft_discovery_open(void)
{
    sft_discovery *scanner = calloc(1, sizeof(*scanner));
    if (scanner == NULL) return NULL;
    scanner->fd = -1;

    scanner->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (scanner->fd < 0) {
        LOGF("[sft-discovery] socket failed errno=%d (%s)\n", errno, strerror(errno));
        free(scanner);
        return NULL;
    }

    int one = 1;
    (void)setsockopt(scanner->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(SFT_BEACON_PORT);

    if (bind(scanner->fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        LOGF("[sft-discovery] bind UDP %u failed errno=%d (%s)\n",
             (unsigned)SFT_BEACON_PORT, errno, strerror(errno));
        close(scanner->fd);
        free(scanner);
        return NULL;
    }

    LOGF("[sft-discovery] listening UDP %u\n", (unsigned)SFT_BEACON_PORT);
    return scanner;
}

int sft_discovery_poll(
    sft_discovery *scanner,
    int timeout_ms,
    sft_device *out)
{
    if (scanner == NULL || scanner->fd < 0 || out == NULL) return -1;

    fd_set set;
    FD_ZERO(&set);
    FD_SET(scanner->fd, &set);

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    int ready = select(scanner->fd + 1, &set, NULL, NULL, &tv);
    if (ready == 0) return 0;
    if (ready < 0) {
        if (errno == EINTR) return 0;
        LOGF("[sft-discovery] select failed errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }

    char buffer[256];
    struct sockaddr_in remote;
    socklen_t remote_len = sizeof(remote);
    ssize_t n = recvfrom(scanner->fd, buffer, sizeof(buffer)-1, 0,
                         (struct sockaddr *)&remote, &remote_len);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        LOGF("[sft-discovery] recvfrom failed errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    buffer[n] = '\0';

    if (!parse_beacon(buffer, &remote, out)) return 0;

    LOGF("[sft-discovery] found host=%s port=%u label=%s\n",
         out->host, (unsigned)out->port, out->label);
    return 1;
}

void sft_discovery_close(sft_discovery *scanner)
{
    if (scanner == NULL) return;
    if (scanner->fd >= 0) close(scanner->fd);
    free(scanner);
}
