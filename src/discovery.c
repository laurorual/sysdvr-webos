#include "sysdvr/discovery.h"
#include "sysdvr/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define SYSDVR_DISCOVERY_PORT 19999
#define DISCOVERY_BUFFER_SIZE 512

struct sysdvr_discovery {
    int fd;
};

static bool supported_protocol(const char *p)
{
    return p != NULL &&
           (strcmp(p, "02") == 0 ||
            strcmp(p, "03") == 0);
}

static char *take_field(char **cursor)
{
    if (cursor == NULL ||
        *cursor == NULL ||
        **cursor == '\0') {
        return NULL;
    }

    char *field = *cursor;
    char *separator = strchr(field, '|');

    if (separator == NULL) {
        *cursor = NULL;
    } else {
        *separator = '\0';
        *cursor = separator + 1;
    }

    return field;
}

static bool parse_descriptor(
    const uint8_t *data,
    size_t size,
    const struct sockaddr_in *remote,
    sysdvr_device *out)
{
    if (data == NULL ||
        out == NULL ||
        remote == NULL ||
        size == 0) {
        return false;
    }

    char text[DISCOVERY_BUFFER_SIZE];

    size_t n =
        size < sizeof(text) - 1
            ? size
            : sizeof(text) - 1;

    memcpy(text, data, n);
    text[n] = '\0';

    size_t actual = 0;
    while (actual < n &&
           text[actual] != '\0') {
        ++actual;
    }
    text[actual] = '\0';

    char *cursor = text;
    char *magic = take_field(&cursor);
    char *client_version = take_field(&cursor);
    char *protocol = take_field(&cursor);

    /*
     * The remainder is the serial. Keeping the remainder intact is safer than
     * relying on strtok_r(..., "", ...), whose empty-delimiter behaviour is a
     * libc extension rather than portable C/POSIX semantics.
     */
    char *serial =
        cursor != NULL && *cursor != '\0'
            ? cursor
            : NULL;

    if (magic == NULL ||
        strcmp(magic, "SysDVR") != 0 ||
        client_version == NULL ||
        protocol == NULL ||
        !supported_protocol(protocol)) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (inet_ntop(
            AF_INET,
            &remote->sin_addr,
            out->host,
            sizeof(out->host)) == NULL) {
        return false;
    }

    snprintf(
        out->client_version,
        sizeof(out->client_version),
        "%s",
        client_version
    );

    snprintf(
        out->protocol,
        sizeof(out->protocol),
        "%s",
        protocol
    );

    if (serial != NULL) {
        snprintf(
            out->serial,
            sizeof(out->serial),
            "%s",
            serial
        );
    }

    return true;
}

sysdvr_discovery *sysdvr_discovery_open(void)
{
    sysdvr_discovery *scanner =
        calloc(1, sizeof(*scanner));

    if (scanner == NULL) {
        return NULL;
    }

    scanner->fd =
        socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_UDP
        );

    if (scanner->fd < 0) {
        free(scanner);
        return NULL;
    }

    int one = 1;
    (void)setsockopt(
        scanner->fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &one,
        sizeof(one)
    );

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr =
        htonl(INADDR_ANY);
    address.sin_port =
        htons(SYSDVR_DISCOVERY_PORT);

    if (bind(
            scanner->fd,
            (struct sockaddr *)&address,
            sizeof(address)) != 0) {
        LOGF(
                "[discovery] bind UDP %d failed: %s\n",
                SYSDVR_DISCOVERY_PORT,
                strerror(errno));

        close(scanner->fd);
        free(scanner);
        return NULL;
    }

    int flags =
        fcntl(
            scanner->fd,
            F_GETFL,
            0
        );

    if (flags >= 0) {
        (void)fcntl(
            scanner->fd,
            F_SETFL,
            flags | O_NONBLOCK
        );
    }

    LOGF(
            "[discovery] listening for SysDVR broadcasts on UDP %d\n",
            SYSDVR_DISCOVERY_PORT);

    return scanner;
}

int sysdvr_discovery_poll(
    sysdvr_discovery *scanner,
    int timeout_ms,
    sysdvr_device *out)
{
    if (scanner == NULL ||
        scanner->fd < 0 ||
        out == NULL) {
        return -1;
    }

    fd_set set;
    FD_ZERO(&set);
    FD_SET(scanner->fd, &set);

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec =
            (timeout_ms % 1000) * 1000,
    };

    int ready =
        select(
            scanner->fd + 1,
            &set,
            NULL,
            NULL,
            &tv
        );

    if (ready == 0) {
        return 0;
    }

    if (ready < 0) {
        if (errno == EINTR) {
            return 0;
        }

        LOGF(
                "[discovery] select failed: %s\n",
                strerror(errno));
        return -1;
    }

    uint8_t buffer[DISCOVERY_BUFFER_SIZE];

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));

    socklen_t remote_size =
        sizeof(remote);

    ssize_t got =
        recvfrom(
            scanner->fd,
            buffer,
            sizeof(buffer),
            0,
            (struct sockaddr *)&remote,
            &remote_size
        );

    if (got < 0) {
        if (errno == EAGAIN ||
            errno == EWOULDBLOCK ||
            errno == EINTR) {
            return 0;
        }

        LOGF(
                "[discovery] recvfrom failed: %s\n",
                strerror(errno));
        return -1;
    }

    if (!parse_descriptor(
            buffer,
            (size_t)got,
            &remote,
            out)) {
        return 0;
    }

    return 1;
}

void sysdvr_discovery_close(
    sysdvr_discovery *scanner)
{
    if (scanner == NULL) {
        return;
    }

    if (scanner->fd >= 0) {
        close(scanner->fd);
    }

    free(scanner);
}
