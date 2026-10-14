/**
 * Unified SDL3 Audio Output Backend (replaces XAudio2)
 *
 * Provides low-latency 48 kHz 16-bit stereo audio output via SDL3's
 * SDL_AudioStream API across Windows, Linux, and macOS.
 */

#include "apu_xaudio2.h"
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21.3 ms per submission */
#define XA2_NUM_BUFS      6      /* ~128 ms max queue cushion */
#define XA2_FRAME_BYTES   (XA2_CHANNELS * (int)sizeof(int16_t))
#define XA2_MAX_QUEUED_BYTES (XA2_NUM_BUFS * XA2_BUF_SAMPLES * XA2_FRAME_BYTES)

static SDL_AudioStream *g_sdl_stream = NULL;
static int              g_xa2_initialized = 0;
static int              g_xa2_frames_written = 0;

static void (*g_capture_mix)(int16_t *stereo, int frames) = NULL;
static SDL_Mutex *g_tap_mutex = NULL;
static FILE      *g_tap = NULL;

void xbox_AudioSetCaptureMix(void (*mix)(int16_t *stereo, int frames))
{
    g_capture_mix = mix;
}

void xbox_AudioRecordTap(const char *path)
{
    if (!g_tap_mutex)
        g_tap_mutex = SDL_CreateMutex();

    SDL_LockMutex(g_tap_mutex);
    if (g_tap) {
        fclose(g_tap);
        g_tap = NULL;
    }
    if (path)
        g_tap = fopen(path, "wb");
    SDL_UnlockMutex(g_tap_mutex);
}

int xa2_init(void)
{
    SDL_AudioSpec spec;

    if (g_xa2_initialized)
        return 1;

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "[AUDIO] SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s\n", SDL_GetError());
            return 0;
        }
    }

    if (!g_tap_mutex)
        g_tap_mutex = SDL_CreateMutex();

    SDL_zero(spec);
    spec.format   = SDL_AUDIO_S16;
    spec.channels = XA2_CHANNELS;
    spec.freq     = XA2_SAMPLE_RATE;

    g_sdl_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_sdl_stream) {
        fprintf(stderr, "[AUDIO] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_ResumeAudioStreamDevice(g_sdl_stream);

    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[AUDIO] SDL3 AudioStream initialized (%d Hz stereo 16-bit)\n", XA2_SAMPLE_RATE);
    return 1;
}

void xa2_shutdown(void)
{
    if (g_sdl_stream) {
        SDL_DestroyAudioStream(g_sdl_stream);
        g_sdl_stream = NULL;
    }
    if (g_xa2_initialized)
        fprintf(stderr, "[AUDIO] SDL3 AudioStream shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int queued_bytes;
    int copy_samples;

    if (!g_xa2_initialized || !g_sdl_stream || num_samples <= 0)
        return 0;

    /* RECOMP_AUDIO_WAV=<path>: record up to 10 minutes of speaker output */
    {
        static FILE *wav = NULL;
        static uint32_t wav_bytes = 0;
        static int wav_state = 0;
        if (!wav_state) {
            const char *pth = getenv("RECOMP_AUDIO_WAV");
            wav_state = 1;
            if (pth && *pth && (wav = fopen(pth, "wb")) != NULL) {
                uint8_t hdr[44] = {
                    'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
                    16,0,0,0, 1,0, 2,0, 0x80,0xBB,0,0, 0x00,0xEE,0x02,0, 4,0, 16,0,
                    'd','a','t','a', 0,0,0,0
                };
                fwrite(hdr, 1, 44, wav);
                char sp[600];
                FILE *sf;
                struct timespec ts;
                timespec_get(&ts, TIME_UTC);
                unsigned long long ms = (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000);
                snprintf(sp, sizeof sp, "%s.start", pth);
                if ((sf = fopen(sp, "w")) != NULL) {
                    fprintf(sf, "%llu\n", ms);
                    fclose(sf);
                }
            }
        }
        if (wav && wav_bytes < 48000u * 4u * 600u) {
            uint32_t n = (uint32_t)num_samples * 4u, riff;
            static int16_t mixbuf[XA2_BUF_SAMPLES * 2];
            if (g_capture_mix && num_samples <= XA2_BUF_SAMPLES) {
                memcpy(mixbuf, samples, n);
                g_capture_mix(mixbuf, num_samples);
                fwrite(mixbuf, 1, n, wav);
            } else {
                fwrite(samples, 1, n, wav);
            }
            wav_bytes += n;
            riff = wav_bytes + 36;
            fseek(wav, 4, SEEK_SET);  fwrite(&riff, 4, 1, wav);
            fseek(wav, 40, SEEK_SET); fwrite(&wav_bytes, 4, 1, wav);
            fseek(wav, 0, SEEK_END);
            fflush(wav);
        }
    }

    if (g_tap && g_tap_mutex) {
        SDL_LockMutex(g_tap_mutex);
        if (g_tap) {
            static int16_t tapbuf[XA2_BUF_SAMPLES * 2];
            if (g_capture_mix && num_samples <= XA2_BUF_SAMPLES) {
                memcpy(tapbuf, samples, (size_t)num_samples * 4u);
                g_capture_mix(tapbuf, num_samples);
                fwrite(tapbuf, 4, (size_t)num_samples, g_tap);
            } else {
                fwrite(samples, 4, (size_t)num_samples, g_tap);
            }
        }
        SDL_UnlockMutex(g_tap_mutex);
    }

    if (getenv("RECOMP_AUDIO_GAPLOG")) {
        static uint32_t run = 0;
        static Uint64 run_t = 0;
        for (int i = 0; i < num_samples; i++) {
            if (samples[2 * i] == 0 && samples[2 * i + 1] == 0) {
                if (!run++) run_t = SDL_GetTicks();
            } else {
                if (run >= 240 && run < 48000u * 5u)
                    fprintf(stderr, "[GAP] %u samples (%.1f ms) t=%llu\n",
                            run, run / 48.0, (unsigned long long)run_t);
                run = 0;
            }
        }
    }

    queued_bytes = SDL_GetAudioStreamQueued(g_sdl_stream);
    if (queued_bytes < 0)
        queued_bytes = 0;

    {
        static unsigned long drops = 0, starves = 0, subs = 0;
        static Uint64 last = 0;
        Uint64 now = SDL_GetTicks();
        subs++;
        if (queued_bytes >= XA2_MAX_QUEUED_BYTES) drops++;
        else if (queued_bytes == 0 && subs > 50) starves++;
        if (now - last >= 10000) {
            fprintf(stderr, "[AUDIO] submits=%lu dropped=%lu underruns=%lu queued_bytes=%d\n",
                    subs, drops, starves, queued_bytes);
            last = now;
        }
    }

    if (queued_bytes >= XA2_MAX_QUEUED_BYTES)
        return 0;

    /* Cushion against underruns when queue drops below 1 buffer (~4KB) */
    if (queued_bytes <= (XA2_BUF_SAMPLES * XA2_FRAME_BYTES)) {
        static const int16_t silence[XA2_BUF_SAMPLES][2] = { 0 };
        SDL_PutAudioStreamData(g_sdl_stream, silence, sizeof(silence));
    }

    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    if (!SDL_PutAudioStreamData(g_sdl_stream, samples, copy_samples * XA2_FRAME_BYTES))
        return 0;

    g_xa2_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}