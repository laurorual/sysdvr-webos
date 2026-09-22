#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ndl_media ndl_media;

typedef enum ndl_feed_result {
    NDL_FEED_OK = 0,
    NDL_FEED_RETRY = 1,
    NDL_FEED_ERROR = 2
} ndl_feed_result;

ndl_media *ndl_media_create(const char *app_id);

/*
 * Opens one NDL DirectMedia v2 timeline containing:
 *   H.264 1280x720
 *   PCM S16LE, interleaved, stereo, 48 kHz
 *
 * Audio and video PTS are therefore interpreted on the same player clock.
 */
bool ndl_media_open(
    ndl_media *player,
    int encoded_width,
    int encoded_height,
    int audio_sample_rate,
    int audio_channels,
    int display_width,
    int display_height
);

ndl_feed_result ndl_media_feed_video(
    ndl_media *player,
    const void *data,
    size_t size,
    uint64_t pts_us
);

ndl_feed_result ndl_media_feed_audio(
    ndl_media *player,
    const void *data,
    size_t size,
    uint64_t pts_us
);

void ndl_media_destroy(ndl_media *player);

#ifdef __cplusplus
}
#endif
