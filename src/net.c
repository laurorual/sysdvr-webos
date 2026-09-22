#include "sysdvr/net.h"
#include "sysdvr/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>


static void peer_to_text(
    int fd,
    char *host,
    size_t host_size,
    unsigned *port_out)
{
    if (host_size > 0) {
        host[0] = '\0';
    }
    if (port_out != NULL) {
        *port_out = 0;
    }

    struct sockaddr_storage address;
    socklen_t address_size = sizeof(address);

    if (getpeername(
            fd,
            (struct sockaddr *)&address,
            &address_size) != 0) {
        snprintf(host, host_size, "?");
        return;
    }

    if (address.ss_family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&address;
        (void)inet_ntop(AF_INET, &v4->sin_addr, host, host_size);
        if (port_out != NULL) {
            *port_out = (unsigned)ntohs(v4->sin_port);
        }
    } else if (address.ss_family == AF_INET6) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&address;
        (void)inet_ntop(AF_INET6, &v6->sin6_addr, host, host_size);
        if (port_out != NULL) {
            *port_out = (unsigned)ntohs(v6->sin6_port);
        }
    } else {
        snprintf(host, host_size, "family-%d", address.ss_family);
    }
}

void sysdvr_log_socket_state(
    int fd,
    const char *context)
{
    int so_error = 0;
    socklen_t so_error_size = sizeof(so_error);

    if (getsockopt(
            fd,
            SOL_SOCKET,
            SO_ERROR,
            &so_error,
            &so_error_size) != 0) {
        so_error = errno;
    }

    char peer[INET6_ADDRSTRLEN] = "?";
    unsigned port = 0;
    peer_to_text(fd, peer, sizeof(peer), &port);

    LOGF(
        "[net] socket-state context=%s fd=%d peer=%s:%u "
        "so_error=%d (%s)\n",
        context != NULL ? context : "?",
        fd,
        peer,
        port,
        so_error,
        so_error == 0 ? "none" : strerror(so_error)
    );
}

int sysdvr_tcp_connect(const char *host, uint16_t port)
{
    char port_string[6];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *it = NULL;
    int fd = -1;

    snprintf(port_string, sizeof(port_string), "%u", (unsigned)port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int gai = getaddrinfo(host, port_string, &hints, &result);
    if (gai != 0) {
        LOGF( "[net] getaddrinfo(%s:%s): %s\n",
                host, port_string, gai_strerror(gai));
        return -1;
    }

    for (it = result; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);

    if (fd < 0) {
        LOGF( "[net] could not connect to %s:%s: %s\n",
                host, port_string, strerror(errno));
    }

    return fd;
}

int sysdvr_set_receive_timeout(int fd, int timeout_ms)
{
    if (timeout_ms <= 0) {
        return -1;
    }

    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    if (setsockopt(
            fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)) != 0) {
        LOGF(
            "[net] failed to set SO_RCVTIMEO fd=%d timeout_ms=%d "
            "errno=%d (%s)\n",
            fd,
            timeout_ms,
            errno,
            strerror(errno)
        );
        return -1;
    }

    return 0;
}

int sysdvr_set_receive_buffer(int fd, int requested_bytes)
{
    if (setsockopt(
            fd,
            SOL_SOCKET,
            SO_RCVBUF,
            &requested_bytes,
            sizeof(requested_bytes)) != 0) {
        return -1;
    }

    int effective = 0;
    socklen_t size = sizeof(effective);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &effective, &size) != 0) {
        return -1;
    }

    return effective;
}

ssize_t sysdvr_read_exact(int fd, void *buffer, size_t size)
{
    uint8_t *ptr = (uint8_t *)buffer;
    size_t total = 0;

    while (total < size) {
        ssize_t got = recv(fd, ptr + total, size - total, 0);

        if (got == 0) {
            if (total > 0) {
                LOGF(
                    "[net] recv EOF mid-read fd=%d progress=%zu/%zu\n",
                    fd,
                    total,
                    size
                );
                sysdvr_log_socket_state(fd, "recv-eof-mid-read");
            }

            /*
             * Returning the partial byte count gives protocol.c enough
             * information to log exactly how far the packet got.
             */
            return (ssize_t)total;
        }

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }

            int recv_errno = errno;
            bool timed_out =
                recv_errno == EAGAIN ||
                recv_errno == EWOULDBLOCK ||
                recv_errno == ETIMEDOUT;

            LOGF(
                "[net] recv %s fd=%d progress=%zu/%zu errno=%d (%s)\n",
                timed_out ? "timeout/failure" : "failure",
                fd,
                total,
                size,
                recv_errno,
                strerror(recv_errno)
            );
            sysdvr_log_socket_state(fd, "recv-error");
            return -1;
        }

        total += (size_t)got;
    }

    return (ssize_t)total;
}

ssize_t sysdvr_write_all(int fd, const void *buffer, size_t size)
{
    const uint8_t *ptr = (const uint8_t *)buffer;
    size_t total = 0;

    while (total < size) {
        ssize_t sent = send(fd, ptr + total, size - total, 0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            LOGF(
                "[net] send failure fd=%d progress=%zu/%zu errno=%d (%s)\n",
                fd,
                total,
                size,
                errno,
                strerror(errno)
            );
            sysdvr_log_socket_state(fd, "send-error");
            return -1;
        }

        if (sent == 0) {
            LOGF(
                "[net] send returned zero fd=%d progress=%zu/%zu\n",
                fd,
                total,
                size
            );
            sysdvr_log_socket_state(fd, "send-zero");
            return -1;
        }

        total += (size_t)sent;
    }

    return (ssize_t)total;
}
