/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#ifdef _WIN32
#include "apu_env.h"     /* cached getenv: debug switches are read per voice */
#endif

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      6   /* ~128 ms: room for host/APU clock drift without drops */

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
/* Recording tap: other audio the process plays itself (the broadcast theme,
 * src/nfl2k5_presentation.cpp) is mixed into the RECOMP_AUDIO_WAV recording
 * at the same moment, so a capture has everything that was heard. */
static void (*g_capture_mix)(int16_t *stereo, int frames);
void xbox_AudioSetCaptureMix(void (*mix)(int16_t *stereo, int frames)) { g_capture_mix = mix; }

/* Video clips (the presenter's xbox_PresentRecord): while a clip records,
 * what goes to the speakers -- 48 kHz stereo s16, the custom-soundtrack mix
 * included -- is appended raw to the clip's audio file. */
static SRWLOCK g_tap_lock = SRWLOCK_INIT;
static FILE *g_tap;
void xbox_AudioRecordTap(const char *path)
{
    AcquireSRWLockExclusive(&g_tap_lock);
    if (g_tap) { fclose(g_tap); g_tap = NULL; }
    if (path) g_tap = fopen(path, "wb");
    ReleaseSRWLockExclusive(&g_tap_lock);
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    /* RECOMP_AUDIO_WAV=<path>: record what is sent to the speakers (up to
     * ten minutes), to check the output without listening. */
    {
        static FILE *wav;
        static uint32_t wav_bytes;
        static int wav_state;
        if (!wav_state) {
            const char *pth = getenv("RECOMP_AUDIO_WAV");
            wav_state = 1;
            if (pth && *pth && (wav = fopen(pth, "wb")) != NULL) {
                uint8_t hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
                                    16,0,0,0, 1,0, 2,0, 0x80,0xBB,0,0, 0x00,0xEE,0x02,0, 4,0, 16,0,
                                    'd','a','t','a', 0,0,0,0 };
                fwrite(hdr, 1, 44, wav);
                {
                    /* Wall-clock start of the recording, to line it up with a
                     * screen capture: <path>.start holds Unix time in ms. */
                    char sp[600];
                    FILE *sf;
                    FILETIME ft;
                    ULARGE_INTEGER t;
                    GetSystemTimeAsFileTime(&ft);
                    t.LowPart = ft.dwLowDateTime; t.HighPart = ft.dwHighDateTime;
                    snprintf(sp, sizeof sp, "%s.start", pth);
                    if ((sf = fopen(sp, "w")) != NULL) {
                        fprintf(sf, "%llu\n", (unsigned long long)((t.QuadPart - 116444736000000000ull) / 10000ull));
                        fclose(sf);
                    }
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
            fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
            fseek(wav, 40, SEEK_SET); fwrite(&wav_bytes, 4, 1, wav);
            fseek(wav, 0, SEEK_END);
            fflush(wav);
        }
    }

    if (g_tap) {
        AcquireSRWLockExclusive(&g_tap_lock);
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
        ReleaseSRWLockExclusive(&g_tap_lock);
    }

    /* RECOMP_AUDIO_GAPLOG: log every run of digital silence of 5 ms or more
     * with GetTickCount, to line output dropouts up with voice events. */
    if (getenv("RECOMP_AUDIO_GAPLOG")) {
        static uint32_t run;
        static DWORD run_t;
        int i;
        for (i = 0; i < num_samples; i++) {
            if (samples[2 * i] == 0 && samples[2 * i + 1] == 0) {
                if (!run++) run_t = GetTickCount();
            } else {
                if (run >= 240 && run < 48000u * 5u)
                    fprintf(stderr, "[GAP] %u samples (%.1f ms) t=%lu\n", run, run / 48.0, (unsigned long)run_t);
                run = 0;
            }
        }
    }

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    /* Dropped buffers (queue full: the APU runs ahead of the speakers) and
     * underruns (queue empty: behind) are both audible clicks; count them. */
    {
        static unsigned long drops, starves, subs;
        static DWORD last;
        DWORD now = GetTickCount();
        subs++;
        if ((int)state.BuffersQueued >= XA2_NUM_BUFS) drops++;
        else if (state.BuffersQueued == 0 && subs > 50) starves++;
        if (now - last >= 10000) {
            fprintf(stderr, "[XA2] submits=%lu dropped=%lu underruns=%lu queued=%u\n",
                    subs, drops, starves, (unsigned)state.BuffersQueued);
            last = now;
        }
    }
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) return 0;

    /* Keep a cushion. The APU makes samples at exactly the output rate, so
     * the queue depth never recovers by itself: it sat at one buffer
     * (~21 ms), and any stall of the frame thread emptied it -- a click.
     * One silent buffer ahead when the queue runs low keeps ~40-60 ms
     * queued from then on (2026-09-26). */
    if (state.BuffersQueued <= 1 && (int)state.BuffersQueued + 2 <= XA2_NUM_BUFS) {
        static const int16_t silence[XA2_BUF_SAMPLES][2];
        memset(&xbuf, 0, sizeof(xbuf));
        xbuf.AudioBytes = XA2_BUF_SAMPLES * XA2_CHANNELS * sizeof(int16_t);
        xbuf.pAudioData = (const BYTE *)silence;
        IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    }

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- POSIX stubs (no audio output yet) */

int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
void xbox_AudioRecordTap(const char *path)          { (void)path; }
int  xa2_get_buffer_size(void)                        { return 0; }

#endif /* _WIN32 */
