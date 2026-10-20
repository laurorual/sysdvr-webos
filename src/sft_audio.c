#include "sft/audio.h"
#include "sysdvr/log.h"

#include <stdio.h>
#include <string.h>

#define SFT_AUDIO_PACKET_KEEP_MS 80u
#define SFT_AUDIO_QUEUE_LIMIT_MS 120u
#define SFT_AUDIO_STALL_RESET_MS 250u

static size_t bytes_for_ms(unsigned ms)
{
    return (size_t)((uint64_t)SFT_AUDIO_BYTES_PER_SECOND * (uint64_t)ms / 1000ULL);
}

static double bytes_to_ms(uint64_t bytes)
{
    return (double)bytes * 1000.0 / (double)SFT_AUDIO_BYTES_PER_SECOND;
}

bool sft_audio_open(sft_audio *a)
{
    if (a == NULL) return false;
    memset(a, 0, sizeof(*a));

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOGF("[sft-audio] SDL audio init failed: %s\n", SDL_GetError());
        a->failed = true;
        return false;
    }

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = (int)SFT_AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = (Uint8)SFT_AUDIO_CHANNELS;
    want.samples = 1024; /* ~21.3 ms at 48 kHz */
    want.callback = NULL; /* SDL_QueueAudio mode */

    SDL_AudioSpec got;
    SDL_zero(got);

    a->device = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (a->device == 0) {
        LOGF("[sft-audio] SDL_OpenAudioDevice failed driver=%s error=%s\n",
             SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
             SDL_GetError());
        a->failed = true;
        return false;
    }

    a->obtained = got;
    if (got.freq != (int)SFT_AUDIO_SAMPLE_RATE ||
        got.format != AUDIO_S16LSB ||
        got.channels != (Uint8)SFT_AUDIO_CHANNELS) {
        LOGF("[sft-audio] unsupported obtained format freq=%d format=0x%x channels=%u\n",
             got.freq, (unsigned)got.format, (unsigned)got.channels);
        SDL_CloseAudioDevice(a->device);
        a->device = 0;
        a->failed = true;
        return false;
    }

    a->ready = true;
    SDL_PauseAudioDevice(a->device, 0);
    a->started = true;

    LOGF("[sft-audio] output ready driver=%s device=%u freq=%d channels=%u samples=%u "
         "queue_limit=%ums packet_tail=%ums\n",
         SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
         (unsigned)a->device,
         got.freq,
         (unsigned)got.channels,
         (unsigned)got.samples,
         SFT_AUDIO_QUEUE_LIMIT_MS,
         SFT_AUDIO_PACKET_KEEP_MS);
    return true;
}

void sft_audio_reset(sft_audio *a, const char *reason)
{
    if (a == NULL || !a->ready || a->device == 0) return;

    Uint32 queued = SDL_GetQueuedAudioSize(a->device);
    if (queued > 0) {
        a->dropped_bytes += queued;
    }
    SDL_ClearQueuedAudio(a->device);
    a->queued_bytes = 0;
    ++a->queue_resets;

    LOGF("[sft-audio] queue reset reason=%s discarded=%.1fms resets=%llu\n",
         reason ? reason : "?",
         bytes_to_ms(queued),
         (unsigned long long)a->queue_resets);
}

bool sft_audio_offer(
    sft_audio *a,
    const void *data,
    size_t size,
    uint64_t received_us)
{
    if (a == NULL || !a->ready || a->failed || a->device == 0 ||
        data == NULL || size == 0) {
        return false;
    }

    ++a->packets;
    a->input_bytes += size;

    const uint8_t *bytes = (const uint8_t *)data;
    size_t keep = size - (size % SFT_AUDIO_BYTES_PER_FRAME);
    if (keep == 0) return true;

    const size_t max_packet = bytes_for_ms(SFT_AUDIO_PACKET_KEEP_MS);
    if (keep > max_packet) {
        size_t drop = keep - max_packet;
        drop -= drop % SFT_AUDIO_BYTES_PER_FRAME;
        bytes += drop;
        keep -= drop;
        a->dropped_bytes += drop;
        LOGF("[sft-audio] trimmed oversized packet input=%.1fms keep=%.1fms dropped=%.1fms\n",
             bytes_to_ms(size), bytes_to_ms(keep), bytes_to_ms(drop));
    }

    if (a->last_offer_us != 0 && received_us > a->last_offer_us) {
        uint64_t gap = received_us - a->last_offer_us;
        if (gap >= (uint64_t)SFT_AUDIO_STALL_RESET_MS * 1000ULL) {
            Uint32 queued = SDL_GetQueuedAudioSize(a->device);
            if (queued > 0) {
                a->dropped_bytes += queued;
                SDL_ClearQueuedAudio(a->device);
                ++a->queue_resets;
                LOGF("[sft-audio] arrival stall %.1fms -> cleared %.1fms queued audio\n",
                     (double)gap / 1000.0, bytes_to_ms(queued));
            }
        }
    }
    a->last_offer_us = received_us;

    Uint32 queued = SDL_GetQueuedAudioSize(a->device);
    const size_t queue_limit = bytes_for_ms(SFT_AUDIO_QUEUE_LIMIT_MS);
    if ((uint64_t)queued + (uint64_t)keep > (uint64_t)queue_limit) {
        a->dropped_bytes += queued;
        SDL_ClearQueuedAudio(a->device);
        ++a->queue_resets;
        LOGF("[sft-audio] latency guard queued=%.1fms incoming=%.1fms -> reset\n",
             bytes_to_ms(queued), bytes_to_ms(keep));
        queued = 0;
    }

    if (SDL_QueueAudio(a->device, bytes, (Uint32)keep) != 0) {
        ++a->queue_errors;
        a->failed = true;
        LOGF("[sft-audio] SDL_QueueAudio failed bytes=%zu error=%s\n",
             keep, SDL_GetError());
        return false;
    }

    queued = SDL_GetQueuedAudioSize(a->device);
    a->queued_bytes = queued;
    if (queued > a->peak_queue_bytes) a->peak_queue_bytes = queued;

    if (a->packets == 1 || (a->packets % 300u) == 0u) {
        LOGF("[sft-audio] packets=%llu input=%.2fMiB queue=%.1fms peak=%.1fms "
             "dropped=%.1fms resets=%llu errors=%llu\n",
             (unsigned long long)a->packets,
             (double)a->input_bytes / (1024.0 * 1024.0),
             bytes_to_ms(queued),
             bytes_to_ms(a->peak_queue_bytes),
             bytes_to_ms(a->dropped_bytes),
             (unsigned long long)a->queue_resets,
             (unsigned long long)a->queue_errors);
    }

    return true;
}

double sft_audio_queued_ms(const sft_audio *a)
{
    if (a == NULL || !a->ready || a->device == 0) return 0.0;
    return bytes_to_ms(SDL_GetQueuedAudioSize(a->device));
}

void sft_audio_log_stats(const sft_audio *a, const char *prefix)
{
    if (a == NULL) return;
    Uint32 queued = (a->ready && a->device != 0) ? SDL_GetQueuedAudioSize(a->device) : 0;
    LOGF("[%s] audio_ready=%d failed=%d packets=%llu input=%.2fMiB queue=%.1fms "
         "peak=%.1fms dropped=%.1fms resets=%llu errors=%llu\n",
         prefix ? prefix : "sft-audio",
         a->ready ? 1 : 0,
         a->failed ? 1 : 0,
         (unsigned long long)a->packets,
         (double)a->input_bytes / (1024.0 * 1024.0),
         bytes_to_ms(queued),
         bytes_to_ms(a->peak_queue_bytes),
         bytes_to_ms(a->dropped_bytes),
         (unsigned long long)a->queue_resets,
         (unsigned long long)a->queue_errors);
}

void sft_audio_close(sft_audio *a)
{
    if (a == NULL) return;
    sft_audio_log_stats(a, "sft-audio-summary");
    if (a->device != 0) {
        SDL_ClearQueuedAudio(a->device);
        SDL_CloseAudioDevice(a->device);
    }
    a->device = 0;
    a->ready = false;
}
