/**
 * Unified SDL3 Audio Output Backend (replaces XAudio2)
 *
 * Provides low-latency 48 kHz 16-bit stereo audio output via SDL3's
 * SDL_AudioStream API across Windows, Linux, and macOS.
 *
 * Local port (2026-10-29) of the 2026-10-22 output fixes from the XAudio2
 * backend (docs/AUDIO-REPORT-HOTKEY-20261022 in the game repo):
 *   - one submission per 256-sample monitor frame (5.3 ms), so the queue fill
 *     is known to 5 ms and the APU paces itself to it (apu_core.c throttle);
 *   - no silence is ever inserted to "cushion" a low queue -- that was a
 *     21 ms hole in the speakers each time, the pacing keeps the queue up;
 *   - the audio report ring (last 90 s + per-second diagnostics) and the
 *     RECOMP_AUDIO_WAV recording written from it on a low-priority thread.
 * RECOMP_AUDIO_LEGACY=1: 6 x 1024-sample buffers, wall-clock pacing.
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
#define XA2_FRAME_BYTES   (XA2_CHANNELS * (int)sizeof(int16_t))
#define XA2_BUF_SAMPLES   256
#define XA2_NUM_BUFS      48  /* 256 ms ceiling; the pacing keeps ~56 ms queued */

static int s_buf = XA2_BUF_SAMPLES, s_nbuf = XA2_NUM_BUFS;
int xa2_legacy(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_AUDIO_LEGACY"); on = e && *e && *e != '0'; }
    return on;
}

static SDL_AudioStream *g_sdl_stream = NULL;
static int              g_xa2_initialized = 0;
static int              g_xa2_frames_written = 0;
static unsigned long    g_xa2_submit_failures = 0;
static int              g_played_last = -1;     /* queued frames at the last query */
static Uint64           g_played_tick;          /* when the queue last moved down */

int xa2_target_samples(void);
#define RPT_SECONDS   90
#define RPT_FRAMES    (RPT_SECONDS * XA2_SAMPLE_RATE)
static int16_t *g_rpt_ring;                   /* RPT_FRAMES stereo frames */

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

    SDL_zero(spec);
    spec.format   = SDL_AUDIO_S16;
    spec.channels = XA2_CHANNELS;
    spec.freq     = XA2_SAMPLE_RATE;

    g_sdl_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_sdl_stream) {
        fprintf(stderr, "[AUDIO] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return 0;
    }
    if (getenv("RECOMP_AUDIO_MUTE")) {      /* background test runs: silent (the mix still runs) */
        SDL_SetAudioStreamGain(g_sdl_stream, 0.0f);
        fprintf(stderr, "[AUDIO] muted (RECOMP_AUDIO_MUTE)\n");
    }

    SDL_ResumeAudioStreamDevice(g_sdl_stream);

    if (xa2_legacy()) { s_buf = 1024; s_nbuf = 6; }
    g_played_last = -1;
    g_played_tick = SDL_GetTicks();
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    g_xa2_submit_failures = 0;

    if (!g_rpt_ring && !getenv("RECOMP_AUDIO_NO_REPORT"))
        g_rpt_ring = (int16_t *)calloc((size_t)RPT_FRAMES, 4u);   /* 17 MB: the last 90 s */
    fprintf(stderr, "[AUDIO] SDL3 AudioStream initialized (%d Hz stereo 16-bit, %d x %d-sample buffers, target %d ms queued, report ring %s)\n",
            XA2_SAMPLE_RATE, s_nbuf, s_buf, xa2_legacy() ? 0 : xa2_target_samples() / 48, g_rpt_ring ? "on" : "off");
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

/* Recording tap: other audio the process plays itself (the broadcast theme,
 * src/nfl2k5_presentation.cpp) is mixed into the RECOMP_AUDIO_WAV recording
 * at the same moment, so a capture has everything that was heard. */
static void (*g_capture_mix)(int16_t *stereo, int frames) = NULL;
void xbox_AudioSetCaptureMix(void (*mix)(int16_t *stereo, int frames)) { g_capture_mix = mix; }

/* Video clips (the presenter's xbox_PresentRecord). */
static SDL_Mutex *g_tap_mutex = NULL;
static FILE      *g_tap = NULL;
void xbox_AudioRecordTap(const char *path)
{
    if (!g_tap_mutex)
        g_tap_mutex = SDL_CreateMutex();
    SDL_LockMutex(g_tap_mutex);
    if (g_tap) { fclose(g_tap); g_tap = NULL; }
    if (path) g_tap = fopen(path, "wb");
    SDL_UnlockMutex(g_tap_mutex);
}

/* The capture mix added to one submission, ONCE (the recording and the tap
 * both write the same mixed buffer). */
static int16_t *g_mixed;
static int g_mixed_cap;
static const int16_t *g_mixed_now;
static const int16_t *capture_mixed(const int16_t *samples, int num_samples)
{
    if (!g_capture_mix) return samples;
    if (g_mixed_now) return g_mixed_now;
    if (num_samples > g_mixed_cap) { free(g_mixed); g_mixed_cap = num_samples; g_mixed = (int16_t *)malloc((size_t)g_mixed_cap * 4u); }
    if (!g_mixed) { g_mixed_cap = 0; return samples; }
    memcpy(g_mixed, samples, (size_t)num_samples * 4u);
    g_capture_mix(g_mixed, num_samples);
    g_mixed_now = g_mixed;
    return g_mixed_now;
}

static uint32_t wav_bytes;
uint32_t xa2_wav_frames(void) { return wav_bytes / 4u; }

/* Speaker-queue statistics since the last call (RECOMP_APU_PITCH_LOG), in buffers. */
static int g_st_qmin, g_st_qmax;
static unsigned g_st_drops, g_st_under, g_st_subs;
void xa2_take_stats(int *qmin, int *qmax, unsigned *drops, unsigned *under, unsigned *subs)
{
    *qmin = g_st_qmin; *qmax = g_st_qmax; *drops = g_st_drops; *under = g_st_under; *subs = g_st_subs;
    g_st_qmin = g_st_qmax = 0; g_st_drops = g_st_under = g_st_subs = 0;
}

static int stream_queued_frames(void)
{
    int b = g_sdl_stream ? SDL_GetAudioStreamQueued(g_sdl_stream) : -1;
    return b < 0 ? 0 : b / XA2_FRAME_BYTES;
}

/* Sample frames queued ahead of the speakers, or -1 when the device has not
 * consumed anything for 250 ms (device lost, endpoint gone): the APU then
 * falls back to the wall clock instead of waiting forever. APU thread only. */
int xa2_queued_samples(void)
{
    int q;
    Uint64 now;
    if (!g_xa2_initialized || !g_sdl_stream) return -1;
    q = stream_queued_frames();
    now = SDL_GetTicks();
    if (q == 0 || g_played_last < 0 || q < g_played_last) {
        g_played_tick = now;
    } else if (now - g_played_tick > 250) {
        g_played_last = q;
        return -1;
    }
    g_played_last = q;
    return q;
}

/* Target queue fill in sample frames: RECOMP_AUDIO_TARGET_MS (default 56 ms, 10-120). */
int xa2_target_samples(void)
{
    static int t;
    if (!t) {
        const char *e = getenv("RECOMP_AUDIO_TARGET_MS");
        int ms = e && *e ? atoi(e) : 56;
        if (ms < 10) ms = 10;
        if (ms > 120) ms = 120;
        t = ms * 48;
    }
    return t;
}

/* ===================================================================
 * Audio report: the last 90 s of what goes to the speakers in a ring, plus
 * one diagnostics record per second.
 * =================================================================== */
static volatile uint64_t g_rpt_pos;           /* frames written in all */
static XA2DiagSec g_rpt_diag[RPT_SECONDS + 2];
static SDL_AtomicInt g_rpt_diag_n;            /* records published in all */
static SDL_AtomicInt g_rpt_busy;
static uint64_t g_rpt_cost_pc, g_rpt_cost_n;

uint64_t xa2_report_frames(void) { return g_rpt_pos; }

static void rpt_write(const int16_t *mixed, int n)
{
    uint64_t pos;
    int at, first;
    if (!g_rpt_ring) return;
    pos = g_rpt_pos;
    at = (int)(pos % RPT_FRAMES);
    first = n < RPT_FRAMES - at ? n : RPT_FRAMES - at;
    memcpy(g_rpt_ring + (size_t)at * 2, mixed, (size_t)first * 4u);
    if (first < n) memcpy(g_rpt_ring, mixed + (size_t)first * 2, (size_t)(n - first) * 4u);
    SDL_MemoryBarrierRelease();
    g_rpt_pos = pos + (uint64_t)n;
}

static int g_dg_qmin = -1, g_dg_qmax;
static unsigned g_dg_under, g_dg_drops;
void xa2_diag_take(int *qmin, int *qmax, unsigned *under, unsigned *drops)
{
    *qmin = g_dg_qmin < 0 ? 0 : g_dg_qmin; *qmax = g_dg_qmax; *under = g_dg_under; *drops = g_dg_drops;
    g_dg_qmin = -1; g_dg_qmax = 0; g_dg_under = g_dg_drops = 0;
}

void xa2_diag_push(const XA2DiagSec *rec)
{
    int n = SDL_GetAtomicInt(&g_rpt_diag_n);
    g_rpt_diag[n % (RPT_SECONDS + 2)] = *rec;
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&g_rpt_diag_n, n + 1);
}

typedef struct { char wav[512], csv[512], txt[512], name[96]; } RptJob;

static void rpt_csv(FILE *f, const XA2DiagSec *r, int nrec, uint64_t first_frame)
{
    int i, k;
    fprintf(f, "wav_seconds,tick_ms,game_fps,worst_frame_ms,apu_vp_frames,queue_min_ms,queue_max_ms,"
               "underruns,dropped,isr_silence_ms,isr_waits,isr_wait_ms,voice_lock_waits,voice_lock_ms,"
               "voices_playing,voices_rate_changed\n");
    for (i = 0; i < nrec; i++) {
        const XA2DiagSec *d = &r[i];
        double ws = ((double)d->frames_out - (double)first_frame) / XA2_SAMPLE_RATE;
        fprintf(f, "%.2f,%lu,%.1f,%.1f,%u,%.1f,%.1f,%u,%u,%.1f,%u,%.1f,%u,%.1f,%u,\"",
                ws, (unsigned long)d->tick, d->fps, d->worst_ms, d->vpframes,
                d->qmin / 48.0, d->qmax / 48.0, d->underruns, d->drops, d->silent_vp * 32 / 48.0,
                d->stalls, d->stall_ms, d->lockwaits, d->lockwait_ms, d->active);
        for (k = 0; k < d->nchg && k < XA2_DIAG_VOICES; k++)
            fprintf(f, "%s%03X:%.0f..%.0fHz/g%.2f", k ? " " : "", d->chg[k].v,
                    d->chg[k].rmin, d->chg[k].rmax, d->chg[k].gain);
        fprintf(f, "\"\n");
    }
}

static int SDLCALL rpt_thread(void *arg)
{
    RptJob *job = (RptJob *)arg;
    uint64_t end = g_rpt_pos, start, n, i;
    int16_t *buf;
    XA2DiagSec *recs;
    int dn = SDL_GetAtomicInt(&g_rpt_diag_n), d0, nrec = 0;
    FILE *f;
    extern void xbox_PresentToast(const char *header, const char *line1, const char *line2, unsigned ms);

    n = end < (uint64_t)(RPT_FRAMES - XA2_SAMPLE_RATE / 2) ? end : (uint64_t)(RPT_FRAMES - XA2_SAMPLE_RATE / 2);
    start = end - n;
    buf = (int16_t *)malloc((size_t)n * 4u + 4u);
    recs = (XA2DiagSec *)malloc(sizeof(XA2DiagSec) * (RPT_SECONDS + 2));
    if (!buf || !recs || !g_rpt_ring) goto out;
    for (i = 0; i < n; ) {
        uint64_t at = (start + i) % RPT_FRAMES, run = RPT_FRAMES - at;
        if (run > n - i) run = n - i;
        memcpy(buf + i * 2, g_rpt_ring + at * 2, (size_t)run * 4u);
        i += run;
    }
    d0 = dn > RPT_SECONDS ? dn - RPT_SECONDS : 0;
    for (; d0 < dn; d0++) {
        const XA2DiagSec *r = &g_rpt_diag[d0 % (RPT_SECONDS + 2)];
        if (r->frames_out < start) continue;
        recs[nrec++] = *r;
    }
    if ((f = fopen(job->wav, "wb")) != NULL) {
        uint32_t bytes = (uint32_t)(n * 4u), riff = bytes + 36;
        uint8_t hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
                            16,0,0,0, 1,0, 2,0, 0x80,0xBB,0,0, 0x00,0xEE,0x02,0, 4,0, 16,0,
                            'd','a','t','a', 0,0,0,0 };
        memcpy(hdr + 4, &riff, 4);
        memcpy(hdr + 40, &bytes, 4);
        fwrite(hdr, 1, 44, f);
        fwrite(buf, 4, (size_t)n, f);
        fclose(f);
    }
    if ((f = fopen(job->csv, "w")) != NULL) {
        rpt_csv(f, recs, nrec, start);
        fclose(f);
    }
    if ((f = fopen(job->txt, "w")) != NULL) {
        unsigned under = 0, drops = 0, low = 0;
        float minfps = 1e9f, worst = 0, sil = 0;
        int k;
        for (k = 0; k < nrec; k++) {
            under += recs[k].underruns; drops += recs[k].drops;
            if (recs[k].fps > 0 && recs[k].fps < minfps) minfps = recs[k].fps;
            if (recs[k].worst_ms > worst) worst = recs[k].worst_ms;
            if (recs[k].fps > 0 && recs[k].fps < 50) low++;
            sil += recs[k].silent_vp * 32 / 48.0f;
        }
        fprintf(f, "NFL2K5-PC audio report %s\n", job->name);
        fprintf(f, "Recording: %s.wav, %.1f s, 48 kHz stereo 16-bit, what went to the speakers (before the master volume).\n",
                job->name, n / 48000.0);
        fprintf(f, "The end of the recording is the moment the report key was pressed.\n\n");
        fprintf(f, "Over these %d seconds:\n", nrec);
        fprintf(f, "  output underruns (speaker queue ran dry): %u\n", under);
        fprintf(f, "  dropped output buffers (queue full):      %u\n", drops);
        fprintf(f, "  silence while waiting for the game (ISR): %.0f ms\n", sil);
        fprintf(f, "  game frame rate: lowest %.0f fps, %u second(s) under 50 fps, worst frame %.0f ms\n",
                minfps > 1e8f ? 0.0f : minfps, low, worst);
        fprintf(f, "  report recording cost: %.2f us per block on the audio thread (capture mix + ring copy)\n",
                g_rpt_cost_n ? (double)g_rpt_cost_pc / (double)g_rpt_cost_n * 1e6 / (double)SDL_GetPerformanceFrequency() : 0.0);
        fprintf(f, "\nPer-second details: %s.csv (voices_rate_changed lists voices whose playback rate moved that second:\n"
                   "voice:min..max Hz/g<loudest gain>; 044/045 in a match are the crowd-reaction layer).\n", job->name);
        fclose(f);
    }
    fprintf(stderr, "[AUDIOREPORT] saved %s (%.1f s, %d diagnostic seconds)\n", job->wav, n / 48000.0, nrec);
    fflush(stderr);
    xbox_PresentToast("AUDIO REPORT SAVED", job->name, "in the logs folder (.wav .csv .txt)", 5000);
out:
    free(buf);
    free(recs);
    free(job);
    SDL_SetAtomicInt(&g_rpt_busy, 0);
    return 0;
}

int xbox_AudioReportSave(char *name_out, size_t n)
{
    extern char *xbox_user_path(const char *rel, char *out, size_t n);
    RptJob *job;
    time_t tt;
    struct tm *t;
    char rel[160];
    SDL_Thread *th;
    if (!g_rpt_ring || !g_rpt_pos) return 0;
    if (!SDL_CompareAndSwapAtomicInt(&g_rpt_busy, 0, 1)) return 0;
    job = (RptJob *)calloc(1, sizeof *job);
    if (!job) { SDL_SetAtomicInt(&g_rpt_busy, 0); return 0; }
    tt = time(NULL);
    t = localtime(&tt);
    snprintf(job->name, sizeof job->name, "audio-report-%04d%02d%02d-%02d%02d%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    SDL_CreateDirectory("logs");
    snprintf(rel, sizeof rel, "logs/%s.wav", job->name); xbox_user_path(rel, job->wav, sizeof job->wav);
    snprintf(rel, sizeof rel, "logs/%s.csv", job->name); xbox_user_path(rel, job->csv, sizeof job->csv);
    snprintf(rel, sizeof rel, "logs/%s.txt", job->name); xbox_user_path(rel, job->txt, sizeof job->txt);
    if (name_out && n) snprintf(name_out, n, "%s", job->name);
    th = SDL_CreateThread(rpt_thread, "audio-report", job);
    if (!th) { free(job); SDL_SetAtomicInt(&g_rpt_busy, 0); return 0; }
    SDL_DetachThread(th);
    return 1;
}

/* RECOMP_AUDIO_WAV with the report ring on: a low-priority thread appends what
 * the ring received since the recording started, four times a second (a disk
 * write on the audio thread stalled the speakers for 20-60 ms at a time). */
static struct { FILE *f; uint64_t start, cap; } g_wav_job;
static int SDLCALL wav_writer_thread(void *arg)
{
    uint64_t done = 0;
    (void)arg;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for (;;) {
        uint64_t end = g_rpt_pos - g_wav_job.start, bytes;
        uint32_t b32, riff;
        if (end > g_wav_job.cap) end = g_wav_job.cap;
        if (end - done > (uint64_t)RPT_FRAMES - XA2_SAMPLE_RATE)
            done = end - ((uint64_t)RPT_FRAMES - XA2_SAMPLE_RATE);
        while (done < end) {
            uint64_t at = (g_wav_job.start + done) % RPT_FRAMES, run = RPT_FRAMES - at;
            if (run > end - done) run = end - done;
            fwrite(g_rpt_ring + at * 2, 4, (size_t)run, g_wav_job.f);
            done += run;
        }
        bytes = done * 4u;
        b32 = (uint32_t)bytes; riff = b32 + 36;
        fseek(g_wav_job.f, 4, SEEK_SET); fwrite(&riff, 4, 1, g_wav_job.f);
        fseek(g_wav_job.f, 40, SEEK_SET); fwrite(&b32, 4, 1, g_wav_job.f);
        fseek(g_wav_job.f, 0, SEEK_END);
        fflush(g_wav_job.f);
        if (done >= g_wav_job.cap) { fclose(g_wav_job.f); return 0; }
        SDL_Delay(250);
    }
}

void xbox_AudioReportRequest(void)
{
    extern void xbox_PresentToast(const char *header, const char *line1, const char *line2, unsigned ms);
    char name[96];
    if (xbox_AudioReportSave(name, sizeof name)) {
        fprintf(stderr, "[AUDIOREPORT] saving %s\n", name);
        xbox_PresentToast("SAVING AUDIO REPORT", name, "the last 90 seconds", 3000);
    } else {
        xbox_PresentToast("AUDIO REPORT", g_rpt_ring ? "still saving the last one" : "not available (no audio output)", "", 3000);
    }
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int qframes, qbufs, copy_samples;
    Uint64 t0;

    g_mixed_now = NULL;
    if (!g_xa2_initialized || !g_sdl_stream || num_samples <= 0)
        return 0;
    t0 = SDL_GetPerformanceCounter();

    /* RECOMP_AUDIO_WAV=<path>: record what is sent to the speakers. */
    {
        static FILE *wav;
        static int wav_state;
        static uint32_t wav_cap;
        if (!wav_state) {
            const char *pth = getenv("RECOMP_AUDIO_WAV");
            wav_state = 1;
            if (pth && *pth && (wav = fopen(pth, "wb")) != NULL) {
                uint8_t hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
                                    16,0,0,0, 1,0, 2,0, 0x80,0xBB,0,0, 0x00,0xEE,0x02,0, 4,0, 16,0,
                                    'd','a','t','a', 0,0,0,0 };
                char sp[600];
                FILE *sf;
                struct timespec ts;
                fwrite(hdr, 1, 44, wav);
#ifdef _WIN32
                timespec_get(&ts, TIME_UTC);
#else
                clock_gettime(CLOCK_REALTIME, &ts);
#endif
                snprintf(sp, sizeof sp, "%s.start", pth);
                if ((sf = fopen(sp, "w")) != NULL) {
                    fprintf(sf, "%llu\n", (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000));
                    fclose(sf);
                }
            }
        }
        if (!wav_cap) {
            const char *cs = getenv("RECOMP_AUDIO_WAV_SECONDS");
            unsigned long secs = cs && *cs ? strtoul(cs, NULL, 10) : 600ul;
            if (secs < 1 || secs > 3600) secs = 600;
            wav_cap = 48000u * 4u * (uint32_t)secs;
        }
        if (wav && g_rpt_ring) {
            static int started;
            if (!started) {
                SDL_Thread *th;
                started = 1;
                g_wav_job.f = wav; g_wav_job.start = g_rpt_pos; g_wav_job.cap = wav_cap / 4u;
                th = SDL_CreateThread(wav_writer_thread, "audio-wav", &g_wav_job);
                if (th) SDL_DetachThread(th);
            }
            {
                uint64_t done = g_rpt_pos + (uint64_t)num_samples - g_wav_job.start;
                wav_bytes = (uint32_t)((done < g_wav_job.cap ? done : g_wav_job.cap) * 4u);
            }
        } else if (wav && wav_bytes < wav_cap) {
            uint32_t n = (uint32_t)num_samples * 4u, riff;
            fwrite(capture_mixed(samples, num_samples), 1, n, wav);
            wav_bytes += n;
            if (wav_bytes % (48000u * 4u) < n) {
                riff = wav_bytes + 36;
                fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
                fseek(wav, 40, SEEK_SET); fwrite(&wav_bytes, 4, 1, wav);
                fseek(wav, 0, SEEK_END);
                fflush(wav);
            }
        }
    }

    if (g_rpt_ring) {
        Uint64 a = SDL_GetPerformanceCounter();
        rpt_write(capture_mixed(samples, num_samples), num_samples);
        g_rpt_cost_pc += SDL_GetPerformanceCounter() - a;
        g_rpt_cost_n++;
    }

    if (g_tap && g_tap_mutex) {
        SDL_LockMutex(g_tap_mutex);
        if (g_tap)
            fwrite(capture_mixed(samples, num_samples), 4, (size_t)num_samples, g_tap);
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

    qframes = stream_queued_frames();
    qbufs = qframes / s_buf;
    {
        if (!g_st_subs || qbufs < g_st_qmin) g_st_qmin = qbufs;
        if (!g_st_subs || qbufs > g_st_qmax) g_st_qmax = qbufs;
        g_st_subs++;
        if (qbufs >= s_nbuf) g_st_drops++;
        else if (qframes == 0) g_st_under++;
        if (g_dg_qmin < 0 || qframes < g_dg_qmin) g_dg_qmin = qframes;
        if (qframes > g_dg_qmax) g_dg_qmax = qframes;
        if (qbufs >= s_nbuf) g_dg_drops++;
        else if (qframes == 0 && g_xa2_frames_written > 50) g_dg_under++;
    }
    {
        static unsigned long drops, starves, low_queue, subs;
        static Uint64 last;
        Uint64 now = SDL_GetTicks();
        subs++;
        if (qbufs >= s_nbuf) drops++;
        else if (qframes == 0 && subs > 50) starves++;
        if (qframes <= 1024) low_queue++;   /* 21 ms or less */
        if (now - last >= 10000) {
            fprintf(stderr, "[AUDIO] submits=%lu dropped=%lu underruns=%lu low_queue=%lu submit_failures=%lu queued_ms=%.1f\n",
                    subs, drops, starves, low_queue, g_xa2_submit_failures, qframes / 48.0);
            last = now;
        }
    }
    if (qbufs >= s_nbuf)
        return 0;

    copy_samples = (num_samples > s_buf) ? s_buf : num_samples;
    if (!SDL_PutAudioStreamData(g_sdl_stream, samples, copy_samples * XA2_FRAME_BYTES)) {
        g_xa2_submit_failures++;
        fprintf(stderr, "[AUDIO] SDL_PutAudioStreamData failed: %s (count=%lu)\n", SDL_GetError(), g_xa2_submit_failures);
        return 0;
    }

    g_xa2_frames_written++;
    {
        Uint64 dt = SDL_GetPerformanceCounter() - t0;
        if (dt * 1000 > 15 * SDL_GetPerformanceFrequency()) {
            static unsigned n;
            if (n++ < 200)
                fprintf(stderr, "[APUSLOW] output %.1f ms\n", (double)dt * 1000.0 / (double)SDL_GetPerformanceFrequency());
        }
    }
    return 1;
}

int xa2_get_buffer_size(void)
{
    return s_buf;
}
