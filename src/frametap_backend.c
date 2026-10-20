#include "sysdvr/stream_backend.h"
#include "sft/audio.h"
#include "sft/discovery.h"
#include "sft/protocol.h"
#include "sysdvr/log.h"
#include "sysdvr/ndl_media.h"
#include "sysdvr/net.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SFT_SOCKET_RCVBUF (4 * 1024 * 1024)
#define SFT_CONNECT_TIMEOUT_MS 3000
#define SFT_FEED_MAX_RETRIES 200
#define SFT_FEED_RETRY_NS 1000000L

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void sleep_ns(long ns)
{
    struct timespec req = {.tv_sec = 0, .tv_nsec = ns};
    nanosleep(&req, NULL);
}

typedef struct sft_worker {
    int fd;
    const char *app_id;
    int display_w;
    int display_h;
    atomic_bool running;
    atomic_bool connected;
    atomic_bool video_started;
    atomic_bool failed;
    atomic_bool finished;

    uint64_t packets;
    uint64_t video_packets;
    uint64_t audio_packets;
    uint64_t status_packets;
    uint64_t video_bytes;
    uint64_t audio_bytes;
    uint64_t keyframes;
    uint64_t sequence_gaps;
    uint64_t source_restarts;
    uint64_t skipped_until_key;
    uint64_t video_age_sum_us;
    uint64_t video_age_max_us;
    uint64_t ndl_retries;
    uint64_t ndl_failures;
    uint64_t max_wall_gap_us;
} sft_worker;

static bool submit_video(
    sft_worker *w,
    ndl_media *player,
    const uint8_t *payload,
    size_t size,
    uint64_t pts_us)
{
    int retries = 0;
    while (atomic_load(&w->running)) {
        ndl_feed_result result = ndl_media_feed_video(player, payload, size, pts_us);
        if (result == NDL_FEED_OK) {
            w->ndl_retries += (uint64_t)retries;
            return true;
        }
        if (result == NDL_FEED_ERROR || retries >= SFT_FEED_MAX_RETRIES) {
            ++w->ndl_failures;
            LOGF("[sft-video] NDL rejected frame pts=%.3fs size=%zu retries=%d\n",
                 (double)pts_us / 1000000.0, size, retries);
            return false;
        }
        ++retries;
        sleep_ns(SFT_FEED_RETRY_NS);
    }
    return false;
}

static void log_status_packet(const uint8_t *payload, size_t size)
{
    if (size < 8) {
        LOGF("[sft-status] packet size=%zu\n", size);
        return;
    }

    uint32_t reason = (uint32_t)payload[4] |
                      ((uint32_t)payload[5] << 8) |
                      ((uint32_t)payload[6] << 16) |
                      ((uint32_t)payload[7] << 24);
    char head[65] = {0};
    if (size > 8) {
        size_t n = size - 8;
        if (n > 64) n = 64;
        memcpy(head, payload + 8, n);
        head[64] = '\0';
    }
    LOGF("[sft-status] reason=%u head=%s\n", reason, head[0] ? head : "<none>");
}

static void *sft_stream_thread(void *opaque)
{
    sft_worker *w = (sft_worker *)opaque;
    uint8_t *payload = malloc(SFT_MAX_PAYLOAD);
    ndl_media *player = NULL;
    sft_audio audio;
    memset(&audio, 0, sizeof(audio));
    bool audio_open_attempted = false;
    uint32_t decoder_w = 0, decoder_h = 0;
    bool waiting_key = true;
    bool have_sequence = false;
    uint32_t last_sequence = 0;
    uint64_t timeline_wall_base = 0;
    uint64_t last_video_wall = 0;
    uint64_t last_key_request = 0;
    uint64_t last_stats_wall = monotonic_us();
    uint64_t last_stats_bytes = 0;

    if (payload == NULL) {
        LOGF("[sft] payload allocation failed\n");
        atomic_store(&w->failed, true);
        atomic_store(&w->finished, true);
        return NULL;
    }

    atomic_store(&w->connected, true);
    (void)sft_request_keyframe(w->fd);
    last_key_request = monotonic_us();
    LOGF("[sft] TCP stream connected; waiting for first IDR/keyframe\n");

    while (atomic_load(&w->running)) {
        sft_packet_header h;
        size_t size = 0;
        int rc = sft_read_packet(w->fd, &w->running, &h, payload, SFT_MAX_PAYLOAD, &size);
        if (rc == 0) {
            if (atomic_load(&w->running)) LOGF("[sft] stream closed by peer\n");
            break;
        }
        if (rc < 0) {
            if (atomic_load(&w->running)) {
                LOGF("[sft] packet read failed\n");
                sysdvr_log_socket_state(w->fd, "Switch Frame Tap stream failure");
                atomic_store(&w->failed, true);
            }
            break;
        }

        ++w->packets;

        if (sft_packet_is_status(&h)) {
            ++w->status_packets;
            log_status_packet(payload, size);
            continue;
        }

        if (sft_packet_is_audio(&h)) {
            ++w->audio_packets;
            w->audio_bytes += size;
            uint64_t audio_now = monotonic_us();
            if (audio.ready) {
                (void)sft_audio_offer(&audio, payload, size, audio_now);
            } else if (!audio_open_attempted && (w->audio_packets % 300u) == 1u) {
                LOGF("[sft-audio] PCM received before outputs are ready packets=%llu bytes=%.2fMiB\n",
                     (unsigned long long)w->audio_packets,
                     (double)w->audio_bytes / (1024.0 * 1024.0));
            }
            continue;
        }

        if (!sft_packet_is_video(&h)) continue;

        ++w->video_packets;
        w->video_bytes += size;
        uint64_t now = monotonic_us();

        if (last_video_wall != 0) {
            uint64_t gap = now - last_video_wall;
            if (gap > w->max_wall_gap_us) w->max_wall_gap_us = gap;
            if (gap >= 100000ULL) {
                LOGF("[diag] sft video arrival gap=%.1fms seq=%u console_age=%.1fms\n",
                     (double)gap / 1000.0, h.sequence,
                     (double)h.console_age_us / 1000.0);
            }
            if (gap >= 500000ULL) {
                sysdvr_log_transport_metrics(w->fd, "video-gap");
            }
        }
        last_video_wall = now;
        w->video_age_sum_us += h.console_age_us;
        if (h.console_age_us > w->video_age_max_us) w->video_age_max_us = h.console_age_us;

        bool key = sft_packet_is_key(&h, payload, size);
        if (key) ++w->keyframes;

        if (have_sequence) {
            uint32_t expected = last_sequence + 1u;
            if (h.sequence != expected) {
                ++w->sequence_gaps;
                if (h.sequence == 0u && key) {
                    ++w->source_restarts;
                    LOGF("[diag] sft source restart expected=%u got=0 key=1 restarts=%llu\n",
                         expected, (unsigned long long)w->source_restarts);
                } else {
                    LOGF("[diag] sft sequence discontinuity expected=%u got=%u key=%d\n",
                         expected, h.sequence, key ? 1 : 0);
                }
                waiting_key = !key;
                if (waiting_key && now - last_key_request >= 250000ULL) {
                    (void)sft_request_keyframe(w->fd);
                    last_key_request = now;
                }
            }
        }
        last_sequence = h.sequence;
        have_sequence = true;

        if (decoder_w != 0 && (h.width != decoder_w || h.height != decoder_h)) {
            LOGF("[sft-video] resolution change %ux%u -> %ux%u seq=%u key=%d\n",
                 decoder_w, decoder_h, h.width, h.height, h.sequence, key ? 1 : 0);
            waiting_key = !key;
            if (!key && now - last_key_request >= 250000ULL) {
                (void)sft_request_keyframe(w->fd);
                last_key_request = now;
            }
        }

        if (waiting_key && !key) {
            ++w->skipped_until_key;
            if (now - last_key_request >= 1000000ULL) {
                (void)sft_request_keyframe(w->fd);
                last_key_request = now;
            }
            continue;
        }

        if (player == NULL || h.width != decoder_w || h.height != decoder_h) {
            if (player != NULL) {
                ndl_media_destroy(player);
                player = NULL;
            }

            player = ndl_media_create(w->app_id);
            if (player == NULL ||
                !ndl_media_open_video_only(player, (int)h.width, (int)h.height,
                                           w->display_w, w->display_h)) {
                LOGF("[sft-video] could not initialize NDL for %ux%u\n", h.width, h.height);
                if (player != NULL) ndl_media_destroy(player);
                player = NULL;
                atomic_store(&w->failed, true);
                break;
            }

            decoder_w = h.width;
            decoder_h = h.height;
            timeline_wall_base = now;
            LOGF("[sft-video] NDL ready encoded=%ux%u display=%dx%d seq=%u\n",
                 decoder_w, decoder_h, w->display_w, w->display_h, h.sequence);

            if (!audio_open_attempted) {
                audio_open_attempted = true;
                if (!sft_audio_open(&audio)) {
                    LOGF("[sft-audio] output unavailable; video will continue\n");
                }
            }
        }

        waiting_key = false;
        uint64_t pts_us = now >= timeline_wall_base ? now - timeline_wall_base : 0;

        if (!submit_video(w, player, payload, size, pts_us)) {
            atomic_store(&w->failed, true);
            break;
        }

        if (!atomic_exchange(&w->video_started, true)) {
            LOGF("[sft-video] first frame submitted seq=%u key=%d size=%zu console_age=%.1fms\n",
                 h.sequence, key ? 1 : 0, size, (double)h.console_age_us / 1000.0);
        }

        if (now - last_stats_wall >= 2000000ULL) {
            uint64_t total_bytes = w->video_bytes + w->audio_bytes;
            uint64_t delta_bytes = total_bytes - last_stats_bytes;
            double seconds = (double)(now - last_stats_wall) / 1000000.0;
            double mbps = seconds > 0.0 ? ((double)delta_bytes * 8.0 / 1000000.0) / seconds : 0.0;
            double avg_age_ms = w->video_packets
                ? (double)w->video_age_sum_us / (double)w->video_packets / 1000.0
                : 0.0;
            LOGF("[sft-stats] video=%llu audio=%llu keys=%llu seq_gaps=%llu restarts=%llu skipped=%llu "
                 "rate=%.1fMbps max_gap=%.1fms age_avg=%.1fms age_max=%.1fms resolution=%ux%u "
                 "audio_q=%.1fms ndl_retries=%llu\n",
                 (unsigned long long)w->video_packets,
                 (unsigned long long)w->audio_packets,
                 (unsigned long long)w->keyframes,
                 (unsigned long long)w->sequence_gaps,
                 (unsigned long long)w->source_restarts,
                 (unsigned long long)w->skipped_until_key,
                 mbps,
                 (double)w->max_wall_gap_us / 1000.0,
                 avg_age_ms,
                 (double)w->video_age_max_us / 1000.0,
                 decoder_w, decoder_h,
                 sft_audio_queued_ms(&audio),
                 (unsigned long long)w->ndl_retries);
            sysdvr_log_transport_metrics(w->fd, "periodic");
            last_stats_wall = now;
            last_stats_bytes = total_bytes;
        }
    }

    if (audio_open_attempted) sft_audio_close(&audio);
    if (player != NULL) ndl_media_destroy(player);
    free(payload);

    LOGF("[sft-summary] packets=%llu video=%llu audio=%llu status=%llu video=%.2fMiB audio=%.2fMiB "
         "keys=%llu seq_gaps=%llu source_restarts=%llu skipped_until_key=%llu ndl_retries=%llu "
         "ndl_failures=%llu max_gap=%.1fms age_avg=%.1fms age_max=%.1fms\n",
         (unsigned long long)w->packets,
         (unsigned long long)w->video_packets,
         (unsigned long long)w->audio_packets,
         (unsigned long long)w->status_packets,
         (double)w->video_bytes / (1024.0 * 1024.0),
         (double)w->audio_bytes / (1024.0 * 1024.0),
         (unsigned long long)w->keyframes,
         (unsigned long long)w->sequence_gaps,
         (unsigned long long)w->source_restarts,
         (unsigned long long)w->skipped_until_key,
         (unsigned long long)w->ndl_retries,
         (unsigned long long)w->ndl_failures,
         (double)w->max_wall_gap_us / 1000.0,
         w->video_packets ? (double)w->video_age_sum_us / (double)w->video_packets / 1000.0 : 0.0,
         (double)w->video_age_max_us / 1000.0);

    atomic_store(&w->finished, true);
    return NULL;
}

static bool action_is_backish(sdl_shell_action action)
{
    return action == SDL_SHELL_ACTION_LEFT || action == SDL_SHELL_ACTION_BACK;
}

session_result frametap_run_stream_session(
    const session_device *device,
    sdl_shell *shell,
    app_ui *ui,
    const char *app_id,
    volatile sig_atomic_t *global_stop)
{
    if (device == NULL || shell == NULL || ui == NULL || app_id == NULL) {
        return SESSION_RESULT_ERROR;
    }

    const char *host = device->info.host;
    LOGF("[sft-session] starting host=%s port=%u\n", host, (unsigned)SFT_STREAM_PORT);

    uint64_t begin = monotonic_us();
    int fd = sysdvr_tcp_connect_timeout(host, SFT_STREAM_PORT, SFT_CONNECT_TIMEOUT_MS);
    uint64_t end = monotonic_us();
    if (fd < 0) {
        LOGF("[sft-net] connect failed host=%s duration=%.1fms\n",
             host, (double)(end - begin) / 1000.0);
        return SESSION_RESULT_TRANSPORT_LOST;
    }

    int rcv = sysdvr_set_receive_buffer(fd, SFT_SOCKET_RCVBUF);
    LOGF("[sft-net] connected fd=%d duration=%.1fms SO_RCVBUF=%d\n",
         fd, (double)(end - begin) / 1000.0, rcv);
    sysdvr_log_socket_state(fd, "Switch Frame Tap connected");
    sysdvr_log_transport_metrics(fd, "connected");

    sft_worker worker;
    memset(&worker, 0, sizeof(worker));
    worker.fd = fd;
    worker.app_id = app_id;
    worker.display_w = shell->width;
    worker.display_h = shell->height;
    atomic_init(&worker.running, true);
    atomic_init(&worker.connected, false);
    atomic_init(&worker.video_started, false);
    atomic_init(&worker.failed, false);
    atomic_init(&worker.finished, false);

    pthread_t thread;
    if (pthread_create(&thread, NULL, sft_stream_thread, &worker) != 0) {
        LOGF("[sft-session] failed to create stream thread\n");
        close(fd);
        return SESSION_RESULT_ERROR;
    }

    bool user_back = false;
    bool ui_cleared = false;
    uint32_t last_wait_render = 0;

    while (!(global_stop && *global_stop) && !atomic_load(&worker.finished)) {
        sdl_shell_action action = SDL_SHELL_ACTION_NONE;
        if (!sdl_shell_poll(shell, &action)) {
            if (global_stop) *global_stop = 1;
            break;
        }

        if (action_is_backish(action)) {
            LOGF("[sft-session] user requested return to console selector\n");
            user_back = true;
            atomic_store(&worker.running, false);
            break;
        }

        if (!ui_cleared && atomic_load(&worker.video_started)) {
            app_ui_clear_for_video(ui);
            ui_cleared = true;
        } else if (!ui_cleared && atomic_load(&worker.connected)) {
            uint32_t now = SDL_GetTicks();
            if (now - last_wait_render >= 500u) {
                app_ui_render_connecting(ui, device, "Connected. Open a game on the Switch...");
                last_wait_render = now;
            }
        }

        SDL_Delay(10);
    }

    atomic_store(&worker.running, false);
    shutdown(fd, SHUT_RDWR);
    pthread_join(thread, NULL);
    close(fd);

    if (user_back) return SESSION_RESULT_USER_BACK;
    if (global_stop && *global_stop) return SESSION_RESULT_REMOTE_END;
    if (atomic_load(&worker.failed)) return SESSION_RESULT_TRANSPORT_LOST;
    return SESSION_RESULT_REMOTE_END;
}
