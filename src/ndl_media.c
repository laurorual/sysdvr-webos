#include "sysdvr/ndl_media.h"
#include "sysdvr/log.h"

#include <NDL_directmedia.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct ndl_media {
    bool initialized;
    bool loaded;

    /*
     * DirectMedia is a single player/timeline. Keep every NDL feed call
     * serialized even though SysDVR receives video and audio on two sockets.
     * This also avoids relying on undocumented thread-safety in vendor code.
     */
    pthread_mutex_t feed_lock;

    uint64_t lock_wait_total_us[2];
    uint64_t call_total_us[2];
    uint64_t lock_wait_max_us[2];
    uint64_t call_max_us[2];
    uint64_t feed_count[2];
    uint64_t slow_lock_count[2];
    uint64_t slow_call_count[2];
};

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}

static void resource_released(const char *type)
{
    LOGF( "[ndl] resource released: %s\n", type ? type : "?");
}

static void media_event(int type, long long value, const char *text)
{
    LOGF( "[ndl] event type=0x%x value=%lld text=%s\n",
            type, value, text ? text : "");
}

ndl_media *ndl_media_create(const char *app_id)
{
    ndl_media *p = calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }

    if (pthread_mutex_init(&p->feed_lock, NULL) != 0) {
        free(p);
        return NULL;
    }

    if (NDL_DirectMediaInit(app_id, resource_released) != 0) {
        LOGF( "[ndl] init failed: %s\n", NDL_DirectMediaGetError());
        pthread_mutex_destroy(&p->feed_lock);
        free(p);
        return NULL;
    }

    p->initialized = true;
    (void)NDL_DirectMediaSetAppState(NDL_DIRECTMEDIA_APP_STATE_FOREGROUND);
    LOGF( "[ndl] DirectMedia v2 initialized as %s\n", app_id);
    return p;
}

bool ndl_media_open(
    ndl_media *p,
    int ew,
    int eh,
    int sample_rate,
    int channels,
    int dw,
    int dh)
{
    if (!p || !p->initialized) {
        return false;
    }

    NDL_DIRECTMEDIA_DATA_INFO_T info = {
        .video = {
            .width = ew,
            .height = eh,
            .type = NDL_VIDEO_TYPE_H264,
        },
        .audio = {
            .pcm = {
                .type = NDL_AUDIO_TYPE_PCM,
                .format = NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_S16LE,
                .layout = "interleaved",
                .channelMode = channels == 1 ? "mono" : "stereo",
                .sampleRate =
                    NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF(sample_rate),
            },
        },
    };

    LOGF(
            "[ndl] loading H264 %dx%d + PCM S16LE %dch %dHz\n",
            ew, eh, channels, sample_rate);

    if (NDL_DirectMediaLoad(&info, media_event) != 0) {
        LOGF( "[ndl] load failed: %s\n", NDL_DirectMediaGetError());
        return false;
    }

    p->loaded = true;

    if (NDL_DirectVideoSetArea(0, 0, dw, dh) != 0) {
        LOGF( "[ndl] set-area warning: %s\n",
                NDL_DirectMediaGetError());
    }

    LOGF( "[ndl] video area 0,0 %dx%d\n", dw, dh);
    return true;
}

static ndl_feed_result feed_locked(
    ndl_media *p,
    const void *data,
    size_t size,
    uint64_t pts_us,
    bool audio)
{
    if (!p || !p->loaded || !data || size == 0) {
        return NDL_FEED_ERROR;
    }

    const unsigned idx = audio ? 1u : 0u;
    const char *kind = audio ? "audio" : "video";

    uint64_t lock_begin = monotonic_us();
    pthread_mutex_lock(&p->feed_lock);
    uint64_t lock_acquired = monotonic_us();

    uint64_t lock_wait = lock_acquired - lock_begin;

    int rc;
    uint64_t call_begin = monotonic_us();

    if (audio) {
        rc = NDL_DirectAudioPlay(
            (void *)data,
            (unsigned int)size,
            (long long)pts_us
        );
    } else {
        rc = NDL_DirectVideoPlay(
            (void *)data,
            (unsigned int)size,
            (long long)pts_us
        );
    }

    uint64_t call_end = monotonic_us();
    uint64_t call_us = call_end - call_begin;

    p->feed_count[idx]++;
    p->lock_wait_total_us[idx] += lock_wait;
    p->call_total_us[idx] += call_us;

    if (lock_wait > p->lock_wait_max_us[idx]) {
        p->lock_wait_max_us[idx] = lock_wait;
    }
    if (call_us > p->call_max_us[idx]) {
        p->call_max_us[idx] = call_us;
    }

    /*
     * 20 ms is deliberately below the 250 ms socket-gap threshold.
     * If DirectMedia is responsible for a visible freeze we want to see
     * the precursor, not merely the later recv() gap.
     */
    if (lock_wait >= 20000ULL) {
        p->slow_lock_count[idx]++;
        LOGF(
                "[diag] NDL %s mutex wait=%.1fms pts=%.3fs\n",
                kind,
                (double)lock_wait / 1000.0,
                (double)pts_us / 1000000.0);
    }

    if (call_us >= 20000ULL) {
        p->slow_call_count[idx]++;
        LOGF(
                "[diag] NDL %s call duration=%.1fms pts=%.3fs rc=%d\n",
                kind,
                (double)call_us / 1000.0,
                (double)pts_us / 1000000.0,
                rc);
    }

    pthread_mutex_unlock(&p->feed_lock);

    return rc == 0 ? NDL_FEED_OK : NDL_FEED_RETRY;
}

ndl_feed_result ndl_media_feed_video(
    ndl_media *p,
    const void *data,
    size_t size,
    uint64_t pts_us)
{
    return feed_locked(p, data, size, pts_us, false);
}

ndl_feed_result ndl_media_feed_audio(
    ndl_media *p,
    const void *data,
    size_t size,
    uint64_t pts_us)
{
    return feed_locked(p, data, size, pts_us, true);
}

void ndl_media_destroy(ndl_media *p)
{
    if (!p) {
        return;
    }

    if (p->loaded) {
        (void)NDL_DirectMediaUnload();
        p->loaded = false;
    }

    if (p->initialized) {
        NDL_DirectMediaQuit();
        p->initialized = false;
    }

    for (unsigned i = 0; i < 2; ++i) {
        const char *kind = i == 0 ? "video" : "audio";
        double avg_lock = p->feed_count[i]
            ? (double)p->lock_wait_total_us[i] / (double)p->feed_count[i] / 1000.0
            : 0.0;
        double avg_call = p->feed_count[i]
            ? (double)p->call_total_us[i] / (double)p->feed_count[i] / 1000.0
            : 0.0;

        LOGF(
                "[ndl] %s timing feeds=%llu avg_lock=%.3fms max_lock=%.1fms "
                "slow_lock=%llu avg_call=%.3fms max_call=%.1fms slow_call=%llu\n",
                kind,
                (unsigned long long)p->feed_count[i],
                avg_lock,
                (double)p->lock_wait_max_us[i] / 1000.0,
                (unsigned long long)p->slow_lock_count[i],
                avg_call,
                (double)p->call_max_us[i] / 1000.0,
                (unsigned long long)p->slow_call_count[i]);
    }

    pthread_mutex_destroy(&p->feed_lock);
    free(p);
}
