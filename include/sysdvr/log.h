#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts one persistent diagnostic log.
 *
 * The target file is truncated on every app launch, so users never have to
 * manage a collection of historical log files. stderr is redirected to the
 * same file so warnings produced by vendor libraries are captured too.
 *
 * Once max_minutes OR max_bytes is reached, stderr is redirected to /dev/null.
 * The application itself continues running normally.
 */
bool sysdvr_log_init(
    const char *path,
    unsigned max_minutes,
    size_t max_bytes
);

void sysdvr_log_printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

void sysdvr_log_close(void);

#define LOGF(...) sysdvr_log_printf(__VA_ARGS__)

#ifdef __cplusplus
}
#endif
