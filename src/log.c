#include "sysdvr/log.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool g_initialized = false;
static bool g_persistent = false;
static bool g_stopped = false;

static uint64_t g_start_us = 0;
static uint64_t g_max_duration_us = 0;
static size_t g_max_bytes = 0;

static char g_path[768];

static uint64_t monotonic_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}

static void timestamp_now(
    char *out,
    size_t out_size)
{
    struct timespec ts;
    struct tm local;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 ||
        localtime_r(&ts.tv_sec, &local) == NULL) {
        snprintf(out, out_size, "0000-00-00 00:00:00.000");
        return;
    }

    snprintf(
        out,
        out_size,
        "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
        local.tm_year + 1900,
        local.tm_mon + 1,
        local.tm_mday,
        local.tm_hour,
        local.tm_min,
        local.tm_sec,
        ts.tv_nsec / 1000000L
    );
}

static size_t current_log_size(void)
{
    struct stat st;

    fflush(stderr);

    if (fstat(STDERR_FILENO, &st) != 0 ||
        st.st_size < 0) {
        return 0;
    }

    return (size_t)st.st_size;
}

static void disable_persistent_logging(
    const char *reason)
{
    if (!g_persistent || g_stopped) {
        return;
    }

    char timestamp[40];
    timestamp_now(timestamp, sizeof(timestamp));

    flockfile(stderr);
    fprintf(
        stderr,
        "[%s] [log] persistent capture stopped: %s; "
        "application will continue normally\n",
        timestamp,
        reason
    );
    fflush(stderr);
    funlockfile(stderr);

    /*
     * Redirect all future stderr — including LG/GStreamer/NDL warnings —
     * away from the capped file.
     */
    if (freopen("/dev/null", "a", stderr) != NULL) {
        setvbuf(stderr, NULL, _IOLBF, 0);
    }

    g_stopped = true;
}

static bool limit_reached(void)
{
    if (!g_persistent || g_stopped) {
        return g_stopped;
    }

    uint64_t now = monotonic_us();

    if (g_max_duration_us != 0 &&
        now >= g_start_us &&
        now - g_start_us >= g_max_duration_us) {
        disable_persistent_logging("time limit reached");
        return true;
    }

    if (g_max_bytes != 0 &&
        current_log_size() >= g_max_bytes) {
        disable_persistent_logging("file size limit reached");
        return true;
    }

    return false;
}

bool sysdvr_log_init(
    const char *path,
    unsigned max_minutes,
    size_t max_bytes)
{
    g_initialized = true;
    g_stopped = false;
    g_persistent = false;
    g_start_us = monotonic_us();

    g_max_duration_us =
        (uint64_t)max_minutes * 60ULL * 1000000ULL;
    g_max_bytes = max_bytes;
    g_path[0] = '\0';

    if (path != NULL && path[0] != '\0') {
        snprintf(g_path, sizeof(g_path), "%s", path);

        /*
         * "w" is intentional: one diagnostic file, always representing the
         * latest app run. It also guarantees an old-version log cannot be
         * mistaken for the currently installed binary.
         */
        if (freopen(path, "w", stderr) != NULL) {
            setvbuf(stderr, NULL, _IOLBF, 0);
            g_persistent = true;
        }
    }

    sysdvr_log_printf(
        "[log] capture started path=%s max_minutes=%u max_bytes=%zu\n",
        g_persistent ? g_path : "<stderr only>",
        max_minutes,
        max_bytes
    );

    return g_persistent;
}

void sysdvr_log_printf(
    const char *format,
    ...)
{
    if (format == NULL || g_stopped) {
        return;
    }

    if (g_initialized && limit_reached()) {
        return;
    }

    char timestamp[40];
    timestamp_now(timestamp, sizeof(timestamp));

    flockfile(stderr);

    fprintf(stderr, "[%s] ", timestamp);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fflush(stderr);
    funlockfile(stderr);
}

void sysdvr_log_close(void)
{
    if (!g_initialized) {
        return;
    }

    if (!g_stopped) {
        sysdvr_log_printf("[log] capture closed normally\n");
    }

    fflush(stderr);

    g_initialized = false;
    g_persistent = false;
}
