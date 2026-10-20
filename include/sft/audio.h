#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SFT_AUDIO_SAMPLE_RATE 48000u
#define SFT_AUDIO_CHANNELS 2u
#define SFT_AUDIO_BYTES_PER_FRAME 4u
#define SFT_AUDIO_BYTES_PER_SECOND (SFT_AUDIO_SAMPLE_RATE * SFT_AUDIO_BYTES_PER_FRAME)

typedef struct sft_audio {
    SDL_AudioDeviceID device;
    SDL_AudioSpec obtained;
    bool ready;
    bool started;
    bool failed;

    uint64_t packets;
    uint64_t input_bytes;
    uint64_t queued_bytes;
    uint64_t dropped_bytes;
    uint64_t queue_resets;
    uint64_t queue_errors;
    uint64_t peak_queue_bytes;
    uint64_t last_offer_us;
} sft_audio;

/*
 * Opens a low-latency PCM S16LE stereo 48 kHz SDL output. Failure is nonfatal:
 * callers may keep video streaming with audio disabled.
 */
bool sft_audio_open(sft_audio *audio);

/*
 * Queue one SFTR audio payload. The pump intentionally bounds latency:
 * - a single oversized packet keeps only its newest ~80 ms tail
 * - long arrival stalls clear already queued audio
 * - device queue growth beyond ~120 ms is flushed before adding fresh PCM
 */
bool sft_audio_offer(
    sft_audio *audio,
    const void *data,
    size_t size,
    uint64_t received_us
);

void sft_audio_reset(sft_audio *audio, const char *reason);

double sft_audio_queued_ms(const sft_audio *audio);

void sft_audio_log_stats(const sft_audio *audio, const char *prefix);

void sft_audio_close(sft_audio *audio);

#ifdef __cplusplus
}
#endif
