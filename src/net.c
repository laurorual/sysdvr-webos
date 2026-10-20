#include "sysdvr/net.h"
#include "sysdvr/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/ioctl.h>
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


void sysdvr_log_transport_metrics(
    int fd,
    const char *context)
{
    int pending = -1;
    if (ioctl(fd, FIONREAD, &pending) != 0) {
        pending = -1;
    }

#ifdef TCP_INFO
    /*
     * Linux TCP_INFO has kept these initial fields ABI-stable for many years,
     * but some older webOS libc headers expose TCP_INFO without declaring
     * struct tcp_info. Use a small compatible prefix instead of depending on
     * the userspace header's struct definition.
     */
    struct sft_tcp_info_compat {
        uint8_t state, ca_state, retransmits, probes;
        uint8_t backoff, options, wscale, app_limited;
        uint32_t rto, ato, snd_mss, rcv_mss;
        uint32_t unacked, sacked, lost, retrans, fackets;
        uint32_t last_data_sent, last_ack_sent, last_data_recv, last_ack_recv;
        uint32_t pmtu, rcv_ssthresh, rtt, rttvar;
        uint32_t snd_ssthresh, snd_cwnd, advmss, reordering;
        uint32_t rcv_rtt, rcv_space;
    } info;
    memset(&info, 0, sizeof(info));
    socklen_t info_size = sizeof(info);

    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &info_size) == 0) {
        LOGF(
            "[transport] context=%s fd=%d rx_pending=%d tcp_state=%u "
            "rtt=%.2fms rttvar=%.2fms rto=%.2fms unacked=%u lost=%u "
            "retrans=%u snd_cwnd=%u rcv_space=%u\n",
            context != NULL ? context : "?",
            fd,
            pending,
            (unsigned)info.state,
            (double)info.rtt / 1000.0,
            (double)info.rttvar / 1000.0,
            (double)info.rto / 1000.0,
            (unsigned)info.unacked,
            (unsigned)info.lost,
            (unsigned)info.retrans,
            (unsigned)info.snd_cwnd,
            (unsigned)info.rcv_space
        );
        return;
    }
#endif

    LOGF(
        "[transport] context=%s fd=%d rx_pending=%d tcp_info=unavailable\n",
        context != NULL ? context : "?",
        fd,
        pending
    );
}

static int connect_one_timeout(
    int fd,
    const struct sockaddr *address,
    socklen_t address_size,
    int timeout_ms)
{
    int old_flags = fcntl(fd, F_GETFL, 0);
    if (old_flags < 0) return -1;
    if (fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) != 0) return -1;

    int rc = connect(fd, address, address_size);
    if (rc != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        (void)fcntl(fd, F_SETFL, old_flags);
        return -1;
    }

    if (rc != 0) {
        fd_set ws;
        FD_ZERO(&ws);
        FD_SET(fd, &ws);
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        rc = select(fd + 1, NULL, &ws, NULL, &tv);
        if (rc <= 0) {
            if (rc == 0) errno = ETIMEDOUT;
            (void)fcntl(fd, F_SETFL, old_flags);
            return -1;
        }

        int so_error = 0;
        socklen_t so_error_size = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_size) != 0 ||
            so_error != 0) {
            if (so_error != 0) errno = so_error;
            (void)fcntl(fd, F_SETFL, old_flags);
            return -1;
        }
    }

    if (fcntl(fd, F_SETFL, old_flags) != 0) return -1;
    return 0;
}

int sysdvr_tcp_connect_timeout(const char *host, uint16_t port, int timeout_ms)
{
    char port_string[6];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *it = NULL;
    int fd = -1;
    int last_errno = 0;

    if (timeout_ms <= 0) timeout_ms = 5000;
    snprintf(port_string, sizeof(port_string), "%u", (unsigned)port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int gai = getaddrinfo(host, port_string, &hints, &result);
    if (gai != 0) {
        LOGF("[net] getaddrinfo(%s:%s): %s\n", host, port_string, gai_strerror(gai));
        return -1;
    }

    for (it = result; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;

        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

        if (connect_one_timeout(fd, it->ai_addr, (socklen_t)it->ai_addrlen, timeout_ms) == 0) {
            break;
        }

        last_errno = errno;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);

    if (fd < 0) {
        if (last_errno != 0) errno = last_errno;
        LOGF("[net] could not connect to %s:%s within %dms: %s\n",
             host, port_string, timeout_ms, strerror(errno));
    }

    return fd;
}

int sysdvr_tcp_connect(const char *host, uint16_t port)
{
    return sysdvr_tcp_connect_timeout(host, port, 5000);
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
