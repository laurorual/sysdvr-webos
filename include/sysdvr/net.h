#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int sysdvr_tcp_connect(const char *host, uint16_t port);

/*
 * Increase the receive queue to make brief decoder stalls less likely to
 * propagate back to the Switch. The kernel may clamp/double the value.
 * Returns the effective SO_RCVBUF value, or -1 on error.
 */
int sysdvr_set_receive_buffer(int fd, int requested_bytes);

/*
 * Sets a defensive receive timeout. A completely wedged network path should
 * eventually unwind the streaming session so the session manager can
 * reconnect instead of blocking forever in recv().
 */
int sysdvr_set_receive_timeout(int fd, int timeout_ms);

/* Emit SO_ERROR and peer endpoint information for post-mortem diagnostics. */
void sysdvr_log_socket_state(int fd, const char *context);

ssize_t sysdvr_read_exact(int fd, void *buffer, size_t size);
ssize_t sysdvr_write_all(int fd, const void *buffer, size_t size);

#ifdef __cplusplus
}
#endif
