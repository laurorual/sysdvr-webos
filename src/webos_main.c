#include "sysdvr/discovery.h"
#include "sysdvr/ndl_media.h"
#include "sysdvr/net.h"
#include "sysdvr/protocol.h"
#include "sysdvr/sdl_shell.h"
#include "sysdvr/session_manager.h"
#include "sysdvr/ui.h"
#include "sysdvr/log.h"

#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define VIDEO_WIDTH 1280
#define VIDEO_HEIGHT 720

#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2

/*
 * Batching=1 means each network transfer contains two 0x1000 PCM chunks.
 * That halves audio packet rate while adding ~21.3 ms over batching=0.
 */
#define AUDIO_BATCHING 1

#define SOCKET_RCVBUF (1024 * 1024)
#define SOCKET_RCVTIMEO_MS 15000

#define FEED_RETRY_NS (2L * 1000L * 1000L)
#define FEED_MAX_RETRIES 250

#define DIAG_GAP_US (250ULL * 1000ULL)
#define DIAG_BACKPRESSURE_US (20ULL * 1000ULL)
#define LOG_CAPTURE_MINUTES 60u
#define LOG_MAX_BYTES (4u * 1024u * 1024u)

static volatile sig_atomic_t g_signal_stop = 0;

static void stop_signal(int sig)
{
    (void)sig;
    g_signal_stop = 1;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}

static void sleep_ns(long ns)
{
    struct timespec t = {
        .tv_sec = ns / 1000000000L,
        .tv_nsec = ns % 1000000000L,
    };

    while (nanosleep(&t, &t) != 0) {
    }
}

static const char *launch_json(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && argv[i][0] == '{') {
            return argv[i];
        }
    }
    return NULL;
}

static bool json_string(
    const char *json,
    const char *key,
    char *out,
    size_t out_size)
{
    if (!json || !key || !out || out_size == 0) {
        return false;
    }

    char needle[96];
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) <= 0) {
        return false;
    }

    const char *p = strstr(json, needle);
    if (!p) {
        return false;
    }

    p += strlen(needle);
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }

    if (*p++ != ':') {
        return false;
    }

    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }

    if (*p++ != '"') {
        return false;
    }

    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            ++p;
        }

        if (n + 1 >= out_size) {
            return false;
        }

        out[n++] = *p++;
    }

    if (*p != '"') {
        return false;
    }

    out[n] = '\0';
    return true;
}

static bool exe_dir(char *out, size_t out_size)
{
    ssize_t n = readlink("/proc/self/exe", out, out_size - 1);
    if (n <= 0 || (size_t)n >= out_size) {
        return false;
    }

    out[n] = '\0';

    char *slash = strrchr(out, '/');
    if (!slash) {
        return false;
    }

    *slash = '\0';
    return true;
}

static bool load_default_host(char *out, size_t out_size)
{
    char dir[PATH_MAX];
    char path[PATH_MAX];

    if (!exe_dir(dir, sizeof(dir))) {
        return false;
    }

    if (snprintf(path, sizeof(path), "%s/switch_ip.txt", dir) <= 0) {
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }

    bool ok = fgets(out, (int)out_size, f) != NULL;
    fclose(f);

    if (!ok) {
        return false;
    }

    out[strcspn(out, "\r\n\t ")] = '\0';
    return out[0] && strcmp(out, "CHANGE_ME") != 0;
}


typedef struct stream_worker {
    const char *name;
    enum sysdvr_stream_kind kind;
    int fd;

    ndl_media *player;
    atomic_bool *running;

    uint64_t timeline_base_us;

    struct sysdvr_packet_header first_header;
    uint8_t *first_payload;
    size_t first_size;
    bool first_pending;

    uint8_t *payload;

    uint64_t packets;
    uint64_t bytes;
    uint64_t last_source_pts_us;
    uint64_t last_rx_wall_us;

    uint64_t gap_events;
    uint64_t max_wall_gap_us;
    uint64_t max_source_gap_us;
    uint64_t backpressure_events;
    uint64_t backpressure_wait_us;

    bool transport_error;
    bool decoder_error;
} stream_worker;

static bool expected_packet(
    enum sysdvr_stream_kind kind,
    const struct sysdvr_packet_header *h)
{
    if (kind == SYSDVR_STREAM_VIDEO) {
        return (h->metadata & SYSDVR_PACKET_VIDEO) != 0u;
    }
    return (h->metadata & SYSDVR_PACKET_AUDIO) != 0u;
}

static int print_error_packet(
    const char *name,
    const uint8_t *payload,
    size_t size)
{
    struct sysdvr_error_packet error;
    if (sysdvr_parse_error_packet(payload, size, &error) != 0) {
        LOGF( "[%s] malformed SysDVR error packet\n", name);
        return -1;
    }

    LOGF(
            "[%s] SysDVR error type=%u code=0x%08x ctx=%llu/%llu/%llu\n",
            name,
            (unsigned)error.error_type,
            (unsigned)error.error_code,
            (unsigned long long)error.context1,
            (unsigned long long)error.context2,
            (unsigned long long)error.context3);
    return -1;
}

static int read_data_packet(
    int fd,
    enum sysdvr_stream_kind kind,
    const char *name,
    struct sysdvr_packet_header *h,
    uint8_t *payload,
    size_t *size)
{
    for (;;) {
        int rc = sysdvr_read_packet(
            fd,
            h,
            payload,
            SYSDVR_MAX_PAYLOAD,
            size
        );

        if (rc <= 0) {
            return rc;
        }

        if ((h->metadata & SYSDVR_PACKET_ERROR) != 0u) {
            return print_error_packet(name, payload, *size);
        }

        if (expected_packet(kind, h) && *size > 0u) {
            return 1;
        }

        LOGF(
                "[%s] ignoring unexpected packet flags=0x%02x size=%zu\n",
                name,
                (unsigned)h->metadata,
                *size);
    }
}

static bool prime_worker(stream_worker *w)
{
    w->first_payload = malloc(SYSDVR_MAX_PAYLOAD);
    w->payload = malloc(SYSDVR_MAX_PAYLOAD);

    if (!w->first_payload || !w->payload) {
        LOGF( "[%s] packet-buffer allocation failed\n", w->name);
        return false;
    }

    int rc = read_data_packet(
        w->fd,
        w->kind,
        w->name,
        &w->first_header,
        w->first_payload,
        &w->first_size
    );

    if (rc != 1) {
        LOGF( "[%s] could not receive initial packet\n", w->name);
        return false;
    }

    w->first_pending = true;

    LOGF(
            "[%s] primed ts=%llu size=%zu flags=0x%02x\n",
            w->name,
            (unsigned long long)w->first_header.timestamp_us,
            w->first_size,
            (unsigned)w->first_header.metadata);

    return true;
}

static ndl_feed_result feed_packet(
    stream_worker *w,
    const void *payload,
    size_t size,
    uint64_t pts_us)
{
    if (w->kind == SYSDVR_STREAM_VIDEO) {
        return ndl_media_feed_video(w->player, payload, size, pts_us);
    }

    return ndl_media_feed_audio(w->player, payload, size, pts_us);
}

static bool submit_packet(
    stream_worker *w,
    const struct sysdvr_packet_header *h,
    const uint8_t *payload,
    size_t size)
{
    uint64_t pts_us =
        h->timestamp_us >= w->timeline_base_us
            ? h->timestamp_us - w->timeline_base_us
            : 0;

    int retries = 0;
    uint64_t retry_start = 0;

    while (atomic_load(w->running)) {
        ndl_feed_result fr = feed_packet(w, payload, size, pts_us);

        if (fr == NDL_FEED_OK) {
            if (retries > 0) {
                uint64_t waited = monotonic_us() - retry_start;
                w->backpressure_events++;
                w->backpressure_wait_us += waited;

                if (waited >= DIAG_BACKPRESSURE_US) {
                    LOGF(
                            "[diag] %s NDL backpressure wait=%.1fms "
                            "retries=%d pts=%.3fs\n",
                            w->name,
                            (double)waited / 1000.0,
                            retries,
                            (double)pts_us / 1000000.0);
                }
            }

            return true;
        }

        if (fr == NDL_FEED_ERROR || retries >= FEED_MAX_RETRIES) {
            LOGF(
                    "[%s] decoder rejected packet pts=%.3fs size=%zu "
                    "retries=%d\n",
                    w->name,
                    (double)pts_us / 1000000.0,
                    size,
                    retries);
            return false;
        }

        if (retries == 0) {
            retry_start = monotonic_us();
        }

        ++retries;
        sleep_ns(FEED_RETRY_NS);
    }

    return false;
}

static void diagnose_arrival(
    stream_worker *w,
    const struct sysdvr_packet_header *h,
    uint64_t now_us)
{
    if (w->last_rx_wall_us != 0) {
        uint64_t wall_delta = now_us - w->last_rx_wall_us;

        uint64_t pts_delta = 0;
        if (h->timestamp_us >= w->last_source_pts_us) {
            pts_delta = h->timestamp_us - w->last_source_pts_us;
        }

        if (wall_delta > w->max_wall_gap_us) {
            w->max_wall_gap_us = wall_delta;
        }
        if (pts_delta > w->max_source_gap_us) {
            w->max_source_gap_us = pts_delta;
        }

        if (wall_delta >= DIAG_GAP_US || pts_delta >= DIAG_GAP_US) {
            ++w->gap_events;

            LOGF(
                    "[diag] %s gap wall=%.1fms source_pts=%.1fms "
                    "packet=%llu\n",
                    w->name,
                    (double)wall_delta / 1000.0,
                    (double)pts_delta / 1000.0,
                    (unsigned long long)(w->packets + 1));
        }
    }

    w->last_rx_wall_us = now_us;
    w->last_source_pts_us = h->timestamp_us;
}

static bool process_one(
    stream_worker *w,
    const struct sysdvr_packet_header *h,
    const uint8_t *payload,
    size_t size,
    uint64_t arrival_us)
{
    diagnose_arrival(w, h, arrival_us);

    if (!submit_packet(w, h, payload, size)) {
        return false;
    }

    ++w->packets;
    w->bytes += size;

    if ((w->packets % 120u) == 0u) {
        uint64_t pts =
            h->timestamp_us >= w->timeline_base_us
                ? h->timestamp_us - w->timeline_base_us
                : 0;

        LOGF(
                "[%s] packets=%llu MiB=%.2f media=%.2fs "
                "gaps=%llu bp=%llu\n",
                w->name,
                (unsigned long long)w->packets,
                (double)w->bytes / (1024.0 * 1024.0),
                (double)pts / 1000000.0,
                (unsigned long long)w->gap_events,
                (unsigned long long)w->backpressure_events);
    }

    return true;
}

static void *stream_thread(void *arg)
{
    stream_worker *w = arg;

    if (w->first_pending) {
        w->first_pending = false;

        if (!process_one(
                w,
                &w->first_header,
                w->first_payload,
                w->first_size,
                monotonic_us())) {
            w->decoder_error = true;
            atomic_store(w->running, false);
            return NULL;
        }
    }

    while (atomic_load(w->running)) {
        struct sysdvr_packet_header h;
        size_t size = 0;

        int rc = read_data_packet(
            w->fd,
            w->kind,
            w->name,
            &h,
            w->payload,
            &size
        );

        uint64_t arrival = monotonic_us();

        if (rc == 0) {
            LOGF( "[%s] stream closed by Switch\n", w->name);
            break;
        }

        if (rc < 0) {
            LOGF(
                "[%s] stream read failed; marking transport for reconnect\n",
                w->name
            );
            sysdvr_log_socket_state(w->fd, w->name);
            w->transport_error = true;
            break;
        }

        if (!process_one(w, &h, w->payload, size, arrival)) {
            w->decoder_error = true;
            break;
        }
    }

    atomic_store(w->running, false);
    return NULL;
}

static void worker_release(stream_worker *w)
{
    free(w->first_payload);
    free(w->payload);
    w->first_payload = NULL;
    w->payload = NULL;
}

static void log_worker_summary(const stream_worker *w)
{
    LOGF(
        "[%s] summary packets=%llu MiB=%.2f gaps=%llu "
        "max_wall_gap=%.1fms max_source_gap=%.1fms "
        "backpressure=%llu bp_wait=%.1fms transport_error=%d decoder_error=%d\n",
        w->name,
        (unsigned long long)w->packets,
        (double)w->bytes / (1024.0 * 1024.0),
        (unsigned long long)w->gap_events,
        (double)w->max_wall_gap_us / 1000.0,
        (double)w->max_source_gap_us / 1000.0,
        (unsigned long long)w->backpressure_events,
        (double)w->backpressure_wait_us / 1000.0,
        w->transport_error ? 1 : 0,
        w->decoder_error ? 1 : 0
    );
}



static bool action_is_backish(sdl_shell_action action)
{
    return action == SDL_SHELL_ACTION_BACK ||
           action == SDL_SHELL_ACTION_LEFT;
}

typedef enum session_result {
    SESSION_RESULT_REMOTE_END = 0,
    SESSION_RESULT_TRANSPORT_LOST = 1,
    SESSION_RESULT_ERROR = 2,
    SESSION_RESULT_USER_BACK = 3
} session_result;

static session_result run_stream_session(
    const session_device *device,
    sdl_shell *shell,
    app_ui *ui,
    const char *app_id)
{
    const char *host = device->info.host;
    uint64_t session_wall_begin = monotonic_us();

    LOGF(
        "[session] starting host=%s serial=%s version=%s protocol=%s\n",
        host,
        device->info.serial[0] ? device->info.serial : "?",
        device->info.client_version[0] ? device->info.client_version : "?",
        device->info.protocol[0] ? device->info.protocol : "?"
    );

    app_ui_clear_for_video(ui);

    ndl_media *player = ndl_media_create(app_id);
    if (!player) {
        return SESSION_RESULT_ERROR;
    }

    if (!ndl_media_open(
            player,
            VIDEO_WIDTH,
            VIDEO_HEIGHT,
            AUDIO_SAMPLE_RATE,
            AUDIO_CHANNELS,
            shell->width,
            shell->height)) {
        ndl_media_destroy(player);
        return SESSION_RESULT_ERROR;
    }

    LOGF(
        "[state] CONNECTING video host=%s port=%u\n",
        host,
        (unsigned)SYSDVR_VIDEO_PORT
    );

    uint64_t video_connect_begin = monotonic_us();
    int video_fd = sysdvr_tcp_connect(host, SYSDVR_VIDEO_PORT);
    uint64_t video_connect_end = monotonic_us();
    if (video_fd < 0) {
        ndl_media_destroy(player);
        return SESSION_RESULT_ERROR;
    }

    LOGF(
        "[net] video TCP connected fd=%d duration=%.1fms\n",
        video_fd,
        (double)(video_connect_end - video_connect_begin) / 1000.0
    );

    int video_buf = sysdvr_set_receive_buffer(video_fd, SOCKET_RCVBUF);
    LOGF("[net] video SO_RCVBUF=%d\n", video_buf);

    char video_protocol[3];
    uint64_t video_handshake_begin = monotonic_us();

    if (sysdvr_handshake(
            video_fd,
            SYSDVR_STREAM_VIDEO,
            0,
            video_protocol) != 0) {
        close(video_fd);
        ndl_media_destroy(player);
        return SESSION_RESULT_ERROR;
    }

    uint64_t video_handshake_end = monotonic_us();

    LOGF(
        "[proto] video handshake OK protocol=%s duration=%.1fms\n",
        video_protocol,
        (double)(video_handshake_end - video_handshake_begin) / 1000.0
    );

    LOGF(
        "[state] CONNECTING audio host=%s port=%u\n",
        host,
        (unsigned)SYSDVR_AUDIO_PORT
    );

    uint64_t audio_connect_begin = monotonic_us();
    int audio_fd = sysdvr_tcp_connect(host, SYSDVR_AUDIO_PORT);
    uint64_t audio_connect_end = monotonic_us();
    if (audio_fd < 0) {
        close(video_fd);
        ndl_media_destroy(player);
        return SESSION_RESULT_ERROR;
    }

    LOGF(
        "[net] audio TCP connected fd=%d duration=%.1fms\n",
        audio_fd,
        (double)(audio_connect_end - audio_connect_begin) / 1000.0
    );

    int audio_buf = sysdvr_set_receive_buffer(audio_fd, SOCKET_RCVBUF);
    LOGF("[net] audio SO_RCVBUF=%d\n", audio_buf);

    char audio_protocol[3];
    uint64_t audio_handshake_begin = monotonic_us();

    if (sysdvr_handshake(
            audio_fd,
            SYSDVR_STREAM_AUDIO,
            AUDIO_BATCHING,
            audio_protocol) != 0) {
        close(audio_fd);
        close(video_fd);
        ndl_media_destroy(player);
        return SESSION_RESULT_ERROR;
    }

    uint64_t audio_handshake_end = monotonic_us();

    LOGF(
        "[proto] audio handshake OK protocol=%s duration=%.1fms\n",
        audio_protocol,
        (double)(audio_handshake_end - audio_handshake_begin) / 1000.0
    );

    atomic_bool running;
    atomic_init(&running, true);

    stream_worker video = {
        .name = "video",
        .kind = SYSDVR_STREAM_VIDEO,
        .fd = video_fd,
        .player = player,
        .running = &running,
    };

    stream_worker audio = {
        .name = "audio",
        .kind = SYSDVR_STREAM_AUDIO,
        .fd = audio_fd,
        .player = player,
        .running = &running,
    };

    if (!prime_worker(&video) || !prime_worker(&audio)) {
        atomic_store(&running, false);
        worker_release(&video);
        worker_release(&audio);
        close(audio_fd);
        close(video_fd);
        ndl_media_destroy(player);
        return SESSION_RESULT_ERROR;
    }

    uint64_t base =
        video.first_header.timestamp_us < audio.first_header.timestamp_us
            ? video.first_header.timestamp_us
            : audio.first_header.timestamp_us;

    video.timeline_base_us = base;
    audio.timeline_base_us = base;

    int64_t initial_av_us =
        (int64_t)audio.first_header.timestamp_us -
        (int64_t)video.first_header.timestamp_us;

    LOGF(
            "[sync] timeline base=%llu video0=%llu audio0=%llu "
            "audio-video=%+.1fms\n",
            (unsigned long long)base,
            (unsigned long long)video.first_header.timestamp_us,
            (unsigned long long)audio.first_header.timestamp_us,
            (double)initial_av_us / 1000.0);

    pthread_t video_thread;
    pthread_t audio_thread;
    bool video_started = false;
    bool audio_started = false;

    if (pthread_create(&video_thread, NULL, stream_thread, &video) != 0) {
        LOGF( "[main] failed to start video thread\n");
        atomic_store(&running, false);
    } else {
        video_started = true;
    }

    if (atomic_load(&running) &&
        pthread_create(&audio_thread, NULL, stream_thread, &audio) == 0) {
        audio_started = true;
    } else if (atomic_load(&running)) {
        LOGF( "[main] failed to start audio thread\n");
        atomic_store(&running, false);
    }

    if (video_started && audio_started) {
        LOGF(
            "[state] STREAMING host=%s video_protocol=%s audio_protocol=%s "
            "LEFT_returns_to_selector=true\n",
            host,
            video_protocol,
            audio_protocol
        );
    }

    bool user_back = false;

    while (atomic_load(&running) && !g_signal_stop) {
        sdl_shell_action action = SDL_SHELL_ACTION_NONE;

        if (!sdl_shell_poll(shell, &action)) {
            g_signal_stop = 1;
            break;
        }

        if (action_is_backish(action)) {
            LOGF(
                    "[session] user requested return to console selector\n");
            user_back = true;
            break;
        }

        SDL_Delay(10);
    }

    atomic_store(&running, false);

    shutdown(video_fd, SHUT_RDWR);
    shutdown(audio_fd, SHUT_RDWR);

    if (video_started) {
        pthread_join(video_thread, NULL);
    }
    if (audio_started) {
        pthread_join(audio_thread, NULL);
    }

    log_worker_summary(&video);
    log_worker_summary(&audio);

    bool transport_lost =
        video.transport_error || audio.transport_error;
    bool decoder_error =
        video.decoder_error || audio.decoder_error;

    if (video.gap_events > 0 && audio.gap_events > 0) {
        LOGF(
            "[diag] both audio and video observed stalls during this session; "
            "shared transport/source instability is likely "
            "video_max_wall=%.1fms audio_max_wall=%.1fms\n",
            (double)video.max_wall_gap_us / 1000.0,
            (double)audio.max_wall_gap_us / 1000.0
        );
    }

    worker_release(&video);
    worker_release(&audio);

    close(audio_fd);
    close(video_fd);

    ndl_media_destroy(player);

    uint64_t session_wall_end = monotonic_us();

    LOGF(
        "[session] ended host=%s transport_lost=%d decoder_error=%d "
        "user_back=%d wall_duration=%.3fs\n",
        host,
        transport_lost ? 1 : 0,
        decoder_error ? 1 : 0,
        user_back ? 1 : 0,
        (double)(session_wall_end - session_wall_begin) / 1000000.0
    );

    if (user_back) {
        return SESSION_RESULT_USER_BACK;
    }

    if (decoder_error) {
        return SESSION_RESULT_ERROR;
    }

    if (transport_lost) {
        LOGF(
            "[reconnect] transport failure is recoverable; "
            "session manager will wait for the selected console\n"
        );
        return SESSION_RESULT_TRANSPORT_LOST;
    }

    return SESSION_RESULT_REMOTE_END;
}

static uint64_t ui_now_ms(void)
{
    return (uint64_t)SDL_GetTicks();
}

static bool poll_discovery_once(
    sysdvr_discovery *scanner,
    session_manager *manager)
{
    if (scanner == NULL) {
        return false;
    }

    for (;;) {
        sysdvr_device device;
        int rc = sysdvr_discovery_poll(scanner, 0, &device);

        if (rc < 0) {
            return false;
        }

        if (rc == 0) {
            return true;
        }

        session_manager_seen(
            manager,
            &device,
            ui_now_ms()
        );
    }
}

static bool browse_for_device(
    sdl_shell *shell,
    app_ui *ui,
    session_manager *manager,
    sysdvr_discovery *scanner)
{
    uint32_t last_render = 0;

    while (!g_signal_stop) {
        sdl_shell_action action = SDL_SHELL_ACTION_NONE;

        if (!sdl_shell_poll(shell, &action)) {
            g_signal_stop = 1;
            return false;
        }

        if (!poll_discovery_once(scanner, manager)) {
            LOGF( "[discovery] scanner error\n");
        }

        session_manager_prune(
            manager,
            ui_now_ms()
        );

        if (action == SDL_SHELL_ACTION_UP) {
            session_manager_move(manager, -1);
        } else if (action == SDL_SHELL_ACTION_DOWN) {
            session_manager_move(manager, 1);
        } else if (action == SDL_SHELL_ACTION_OK &&
                   manager->count > 0) {
            session_manager_remember_selected(manager);
            return true;
        } else if (action_is_backish(action)) {
            g_signal_stop = 1;
            return false;
        }

        uint32_t now = SDL_GetTicks();
        if (now - last_render >= 80u) {
            app_ui_render_browser(
                ui,
                manager,
                scanner != NULL
            );
            last_render = now;
        }

        SDL_Delay(15);
    }

    return false;
}

static bool wait_for_preferred_console(
    sdl_shell *shell,
    app_ui *ui,
    session_manager *manager,
    sysdvr_discovery *scanner,
    session_device *out_device)
{
    uint32_t last_render = 0;

    while (!g_signal_stop) {
        sdl_shell_action action = SDL_SHELL_ACTION_NONE;

        if (!sdl_shell_poll(shell, &action)) {
            g_signal_stop = 1;
            return false;
        }

        if (action_is_backish(action)) {
            LOGF(
                    "[reconnect] user cancelled automatic reconnect (LEFT/Back)\n");
            return false;
        }

        if (scanner != NULL) {
            for (;;) {
                sysdvr_device found;
                int rc = sysdvr_discovery_poll(scanner, 0, &found);

                if (rc <= 0) {
                    break;
                }

                session_manager_seen(
                    manager,
                    &found,
                    ui_now_ms()
                );

                if (session_manager_matches_preferred(
                        manager,
                        &found)) {
                    memset(out_device, 0, sizeof(*out_device));
                    out_device->info = found;
                    out_device->last_seen_ms = ui_now_ms();

                    LOGF(
                            "[reconnect] preferred console returned at %s\n",
                            found.host);

                    return true;
                }
            }
        }

        session_manager_prune(
            manager,
            ui_now_ms()
        );

        uint32_t now = SDL_GetTicks();
        if (now - last_render >= 80u) {
            app_ui_render_reconnect(
                ui,
                manager
            );
            last_render = now;
        }

        SDL_Delay(15);
    }

    return false;
}

static void show_error_briefly(
    sdl_shell *shell,
    app_ui *ui,
    const char *title,
    const char *detail)
{
    app_ui_render_error(
        ui,
        title,
        detail
    );

    uint32_t start = SDL_GetTicks();

    while (!g_signal_stop &&
           SDL_GetTicks() - start < 1600u) {
        sdl_shell_action action = SDL_SHELL_ACTION_NONE;

        if (!sdl_shell_poll(shell, &action)) {
            g_signal_stop = 1;
            return;
        }

        if (action_is_backish(action)) {
            return;
        }

        SDL_Delay(20);
    }
}

int main(int argc, char **argv)
{
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);

    const char *params = launch_json(argc, argv);

    char log_path[PATH_MAX] = {0};

    if (!json_string(
            params,
            "log",
            log_path,
            sizeof(log_path))) {
        char app_directory[PATH_MAX];

        if (exe_dir(
                app_directory,
                sizeof(app_directory))) {
            (void)snprintf(
                log_path,
                sizeof(log_path),
                "%s/sysdvr-webos.log",
                app_directory
            );
        } else {
            snprintf(
                log_path,
                sizeof(log_path),
                "%s",
                "/tmp/sysdvr-webos.log"
            );
        }
    }

    (void)sysdvr_log_init(
        log_path,
        LOG_CAPTURE_MINUTES,
        LOG_MAX_BYTES
    );

    LOGF("=== sysdvr-webos %s launch ===\n", SYSDVR_APP_VERSION);
    LOGF(
        "[main] pid=%ld target=H264 1280x720 + PCM S16LE stereo 48000Hz "
        "audio_batching=%d\n",
        (long)getpid(),
        AUDIO_BATCHING
    );
    LOGF(
        "[main] diagnostic policy: one file per launch, "
        "capture=%umin, size_cap=%uMiB\n",
        LOG_CAPTURE_MINUTES,
        (unsigned)(LOG_MAX_BYTES / (1024u * 1024u))
    );

    if (!sdl_shell_preinit()) {
        return 1;
    }

    sdl_shell shell;
    if (!sdl_shell_open(&shell, "SysDVR")) {
        SDL_Quit();
        return 1;
    }

    app_ui ui;
    if (!app_ui_init(&ui, shell.renderer)) {
        LOGF(
                "[main] UI initialization failed\n");
        sdl_shell_close(&shell);
        return 1;
    }

    const char *app_id = getenv("APPID");
    if (!app_id || !*app_id) {
        app_id = "io.github.sysdvrwebos.client";
    }

    session_manager manager;
    session_manager_init(&manager);

    /*
     * Fixed host support is kept as a manual entry. Unlike 0.4 it still
     * appears in the selector instead of auto-connecting.
     */
    char configured_host[256] = {0};
    bool have_manual_host =
        json_string(
            params,
            "host",
            configured_host,
            sizeof(configured_host)
        ) ||
        load_default_host(
            configured_host,
            sizeof(configured_host)
        );

    if (have_manual_host) {
        session_manager_add_manual(
            &manager,
            configured_host
        );
    }

    sysdvr_discovery *scanner =
        sysdvr_discovery_open();

    if (scanner == NULL) {
        LOGF(
                "[discovery] unavailable; manual host entries only\n");
    }

    LOGF(
            "[state] SELECTOR ready; UP/DOWN navigate, OK connects, LEFT closes/returns\n");

    while (!g_signal_stop) {
        if (!browse_for_device(
                &shell,
                &ui,
                &manager,
                scanner)) {
            break;
        }

        const session_device *selected =
            session_manager_selected(&manager);

        if (selected == NULL) {
            continue;
        }

        session_device target = *selected;

        LOGF(
                "[session] user selected host=%s serial=%s\n",
                target.info.host,
                target.info.serial[0]
                    ? target.info.serial
                    : "?");

        app_ui_render_connecting(
            &ui,
            &target,
            "Connecting to Nintendo Switch..."
        );

        SDL_Delay(180);

        session_result result =
            run_stream_session(
                &target,
                &shell,
                &ui,
                app_id
            );

        if (g_signal_stop) {
            break;
        }

        if (result == SESSION_RESULT_USER_BACK) {
            LOGF(
                    "[session] returning to selector by user request\n");
            app_ui_render_browser(
                &ui,
                &manager,
                scanner != NULL
            );
            continue;
        }

        if (result == SESSION_RESULT_ERROR) {
            show_error_briefly(
                &shell,
                &ui,
                "Could not start the stream",
                "Check SysDVR and the network connection."
            );

            continue;
        }

        /*
         * Clean EOF (sleep/restart) and mid-stream transport failures both
         * enter the same preferred-console reconnect state.
         */
        LOGF(
            "[state] WAIT_RECONNECT reason=%s; waiting for selected console\n",
            result == SESSION_RESULT_TRANSPORT_LOST
                ? "transport-lost"
                : "remote-end"
        );

        session_device returned;

        if (wait_for_preferred_console(
                &shell,
                &ui,
                &manager,
                scanner,
                &returned)) {
            /*
             * Refresh the registry entry first. Then select the matching item
             * so the outer loop's next manual browser state remains coherent.
             */
            session_manager_seen(
                &manager,
                &returned.info,
                ui_now_ms()
            );

            /*
             * Reconnect immediately without asking the user to select again.
             */
            session_device reconnect_target = returned;

            while (!g_signal_stop) {
                app_ui_render_connecting(
                    &ui,
                    &reconnect_target,
                    "Reconnecting to Nintendo Switch..."
                );

                SDL_Delay(180);

                session_result reconnect_result =
                    run_stream_session(
                        &reconnect_target,
                        &shell,
                        &ui,
                        app_id
                    );

                if (g_signal_stop) {
                    break;
                }

                if (reconnect_result ==
                    SESSION_RESULT_USER_BACK) {
                    break;
                }

                if (reconnect_result ==
                    SESSION_RESULT_ERROR) {
                    show_error_briefly(
                        &shell,
                        &ui,
                        "Connection error",
                        "Waiting for the selected console again."
                    );
                } else if (reconnect_result ==
                           SESSION_RESULT_TRANSPORT_LOST) {
                    LOGF(
                        "[reconnect] transient transport failure during "
                        "reconnected session; waiting for console again\n"
                    );
                }

                if (!wait_for_preferred_console(
                        &shell,
                        &ui,
                        &manager,
                        scanner,
                        &reconnect_target)) {
                    break;
                }
            }
        }

        app_ui_render_browser(
            &ui,
            &manager,
            scanner != NULL
        );
    }

    if (scanner != NULL) {
        sysdvr_discovery_close(scanner);
    }

    app_ui_destroy(&ui);
    sdl_shell_close(&shell);

    LOGF("[state] EXIT normal\n");
    sysdvr_log_close();
    return 0;
}
