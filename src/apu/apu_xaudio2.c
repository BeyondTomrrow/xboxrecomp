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
/* One submission per 256-sample monitor frame (5.3 ms), so the queue fill is
 * known to 5 ms and the APU can be paced by it (apu_core.c throttle). It used
 * to be 1024-sample buffers paced by the wall clock alone, which left the
 * speakers 0-2 buffers ahead and starved 26-34 times per 10 minutes whenever
 * the pipeline paused for 20-40 ms (AUDIO-FIX-20261021, dropouts). */
#define XA2_BUF_SAMPLES   256
#define XA2_NUM_BUFS      48  /* 256 ms ceiling; the pacing keeps ~56 ms queued */
/* RECOMP_AUDIO_LEGACY=1: the old output path for comparison -- 6 x 1024-sample
 * buffers, the APU paced by the wall clock, its thread at normal priority. */
static int s_buf = XA2_BUF_SAMPLES, s_nbuf = XA2_NUM_BUFS;
int xa2_legacy(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_AUDIO_LEGACY"); on = e && *e && *e != '0'; }
    return on;
}

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_store[XA2_NUM_BUFS * XA2_BUF_SAMPLES * 2];   /* s_nbuf x s_buf frames */
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static unsigned long           g_xa2_submit_failures = 0;
static uint64_t                g_xa2_submitted = 0;   /* sample frames handed to the voice */
static uint64_t                g_xa2_played_last;     /* SamplesPlayed at the last query */
static DWORD                   g_xa2_played_tick;     /* when SamplesPlayed last advanced */
static LONGLONG rpt_qpc_freq(void)
{
    static LONGLONG f;
    if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = q.QuadPart; }
    return f;
}
int xa2_target_samples(void);
#define RPT_SECONDS   90
#define RPT_FRAMES    (RPT_SECONDS * XA2_SAMPLE_RATE)
static int16_t *g_rpt_ring;                   /* RPT_FRAMES stereo frames */

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
    if (getenv("RECOMP_AUDIO_MUTE")) {      /* background test runs: silent (the mix still runs) */
        g_xa2_master->lpVtbl->SetVolume(g_xa2_master, 0.0f, 0);
        fprintf(stderr, "[XA2] muted (RECOMP_AUDIO_MUTE)\n");
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
    if (xa2_legacy()) { s_buf = 1024; s_nbuf = 6; }
    g_xa2_submitted = 0;
    g_xa2_played_last = 0;
    g_xa2_played_tick = GetTickCount();
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    g_xa2_submit_failures = 0;

    if (!g_rpt_ring && !getenv("RECOMP_AUDIO_NO_REPORT"))
        g_rpt_ring = (int16_t *)calloc((size_t)RPT_FRAMES, 4u);   /* 17 MB: the last 90 s */
    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers, target %d ms queued, report ring %s)\n",
            XA2_SAMPLE_RATE, s_nbuf, s_buf, xa2_legacy() ? 0 : xa2_target_samples() / 48, g_rpt_ring ? "on" : "off");
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

/* The capture mix (theme, front-end voice) added to one submission, ONCE: the WAV recording and
 * the video-clip tap both write the same mixed buffer (two calls used to consume the mixers'
 * positions twice, so each recording got every other chunk), any size. */
static int16_t *g_mixed;                     /* this submission's capture mix, made once */
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

/* Bytes in the RECOMP_AUDIO_WAV recording so far: the [PITCH] log lines carry the
 * recording position, so log and capture line up exactly. */
static uint32_t wav_bytes;
uint32_t xa2_wav_frames(void) { return wav_bytes / 4u; }

/* Speaker-queue statistics since the last call (RECOMP_APU_PITCH_LOG). */
static int g_st_qmin, g_st_qmax;
static unsigned g_st_drops, g_st_under, g_st_subs;
void xa2_take_stats(int *qmin, int *qmax, unsigned *drops, unsigned *under, unsigned *subs)
{
    *qmin = g_st_qmin; *qmax = g_st_qmax; *drops = g_st_drops; *under = g_st_under; *subs = g_st_subs;
    g_st_qmin = g_st_qmax = 0; g_st_drops = g_st_under = g_st_subs = 0;
}

/* Sample frames queued ahead of the speakers (submitted - played), or -1 when
 * the voice has not consumed anything for 250 ms (device lost, endpoint gone):
 * the APU then falls back to the wall clock instead of waiting forever.
 * Called on the APU thread only. */
int xa2_queued_samples(void)
{
    XAUDIO2_VOICE_STATE st;
    DWORD now;
    if (!g_xa2_initialized || !g_xa2_source) return -1;
    IXAudio2SourceVoice_GetState(g_xa2_source, &st, 0);
    now = GetTickCount();
    if (st.SamplesPlayed != g_xa2_played_last || st.BuffersQueued == 0) {
        g_xa2_played_last = st.SamplesPlayed;
        g_xa2_played_tick = now;
    } else if (now - g_xa2_played_tick > 250) {
        return -1;
    }
    if (g_xa2_submitted <= st.SamplesPlayed) return 0;
    return (int)(g_xa2_submitted - st.SamplesPlayed);
}

/* Target queue fill in sample frames: RECOMP_AUDIO_TARGET_MS (default 56 ms,
 * 10-120). Above the 40 ms the pipeline may wait for the title's ISR before it
 * falls back to silent frames, so such a wait never empties the queue. */
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
 * Audio report ("save what I just heard", AUDIO-REPORT-HOTKEY-20261022):
 * the last 90 s of what goes to the speakers (after the mix, the capture mix
 * included, before the master volume) in a ring, plus one diagnostics record
 * per second. xbox_AudioReportSave() writes both to the user's logs folder
 * on its own thread. The APU thread only copies 1 KB per 5.3 ms into the
 * ring and publishes the position; it never takes a lock for the report.
 * =================================================================== */
static volatile LONG64 g_rpt_pos;             /* frames written in all */
static XA2DiagSec g_rpt_diag[RPT_SECONDS + 2];
static volatile LONG g_rpt_diag_n;            /* records published in all */
static volatile LONG g_rpt_busy;
static uint64_t g_rpt_cost_qpc, g_rpt_cost_n; /* writer cost, for the report */

uint64_t xa2_report_frames(void) { return (uint64_t)g_rpt_pos; }

static void rpt_write(const int16_t *mixed, int n)
{
    uint64_t pos;
    int at, first;
    if (!g_rpt_ring) return;
    pos = (uint64_t)g_rpt_pos;
    at = (int)(pos % RPT_FRAMES);
    first = n < RPT_FRAMES - at ? n : RPT_FRAMES - at;
    memcpy(g_rpt_ring + (size_t)at * 2, mixed, (size_t)first * 4u);
    if (first < n) memcpy(g_rpt_ring, mixed + (size_t)first * 2, (size_t)(n - first) * 4u);
    InterlockedExchange64(&g_rpt_pos, (LONG64)(pos + (uint64_t)n));
}

/* Output-queue statistics for the per-second record (separate from the
 * RECOMP_APU_PITCH_LOG ones so both can run). APU thread only. */
static int g_dg_qmin = -1, g_dg_qmax;
static unsigned g_dg_under, g_dg_drops;
void xa2_diag_take(int *qmin, int *qmax, unsigned *under, unsigned *drops)
{
    *qmin = g_dg_qmin < 0 ? 0 : g_dg_qmin; *qmax = g_dg_qmax; *under = g_dg_under; *drops = g_dg_drops;
    g_dg_qmin = -1; g_dg_qmax = 0; g_dg_under = g_dg_drops = 0;
}

void xa2_diag_push(const XA2DiagSec *rec)
{
    LONG n = g_rpt_diag_n;
    g_rpt_diag[n % (RPT_SECONDS + 2)] = *rec;
    MemoryBarrier();
    InterlockedExchange(&g_rpt_diag_n, n + 1);
}

typedef struct { char wav[MAX_PATH], csv[MAX_PATH], txt[MAX_PATH], name[96]; } RptJob;

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

static DWORD WINAPI rpt_thread(LPVOID arg)
{
    RptJob *job = (RptJob *)arg;
    uint64_t end = (uint64_t)g_rpt_pos, start, n, i;
    int16_t *buf;
    XA2DiagSec *recs;
    LONG dn = g_rpt_diag_n, d0, nrec = 0;
    FILE *f;
    extern void xbox_PresentToast(const char *header, const char *line1, const char *line2, unsigned ms);

    /* The writer moves on while this copies: leave the oldest half second out. */
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
        LONG k;
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
        fprintf(f, "Over these %ld seconds:\n", (long)nrec);
        fprintf(f, "  output underruns (speaker queue ran dry): %u\n", under);
        fprintf(f, "  dropped output buffers (queue full):      %u\n", drops);
        fprintf(f, "  silence while waiting for the game (ISR): %.0f ms\n", sil);
        fprintf(f, "  game frame rate: lowest %.0f fps, %u second(s) under 50 fps, worst frame %.0f ms\n",
                minfps > 1e8f ? 0.0f : minfps, low, worst);
        fprintf(f, "  report recording cost: %.2f us per 5.3 ms block on the audio thread (capture mix + ring copy)\n",
                g_rpt_cost_n ? (double)g_rpt_cost_qpc / (double)g_rpt_cost_n * 1e6 / (double)rpt_qpc_freq() : 0.0);
        fprintf(f, "\nPer-second details: %s.csv (voices_rate_changed lists voices whose playback rate moved that second:\n"
                   "voice:min..max Hz/g<loudest gain>; 044/045 in a match are the crowd-reaction layer).\n", job->name);
        fclose(f);
    }
    fprintf(stderr, "[AUDIOREPORT] saved %s (%.1f s, %ld diagnostic seconds)\n", job->wav, n / 48000.0, (long)nrec);
    fflush(stderr);
    xbox_PresentToast("AUDIO REPORT SAVED", job->name, "in the logs folder (.wav .csv .txt)", 5000);
out:
    free(buf);
    free(recs);
    free(job);
    InterlockedExchange(&g_rpt_busy, 0);
    return 0;
}

/* Starts writing the report; returns 1 (name in name_out) or 0 when one is
 * still being written or nothing has played yet. Any thread. */
int xbox_AudioReportSave(char *name_out, size_t n)
{
    extern char *xbox_user_path(const char *rel, char *out, size_t n);
    RptJob *job;
    SYSTEMTIME t;
    char rel[160];
    HANDLE th;
    if (!g_rpt_ring || !g_rpt_pos) return 0;
    if (InterlockedExchange(&g_rpt_busy, 1)) return 0;
    job = (RptJob *)calloc(1, sizeof *job);
    if (!job) { InterlockedExchange(&g_rpt_busy, 0); return 0; }
    GetLocalTime(&t);
    snprintf(job->name, sizeof job->name, "audio-report-%04u%02u%02u-%02u%02u%02u",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    CreateDirectoryA("logs", NULL);           /* portable layout: relative to the root */
    snprintf(rel, sizeof rel, "logs\\%s.wav", job->name); xbox_user_path(rel, job->wav, sizeof job->wav);
    snprintf(rel, sizeof rel, "logs\\%s.csv", job->name); xbox_user_path(rel, job->csv, sizeof job->csv);
    snprintf(rel, sizeof rel, "logs\\%s.txt", job->name); xbox_user_path(rel, job->txt, sizeof job->txt);
    if (name_out && n) snprintf(name_out, n, "%s", job->name);
    th = CreateThread(NULL, 0, rpt_thread, job, 0, NULL);
    if (!th) { free(job); InterlockedExchange(&g_rpt_busy, 0); return 0; }
    SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(th);
    return 1;
}

/* RECOMP_AUDIO_WAV with the report ring on: a low-priority thread appends what
 * the ring received since the recording started, four times a second. */
static struct { FILE *f; uint64_t start, cap; } g_wav_job;
static DWORD WINAPI wav_writer_thread(LPVOID arg)
{
    uint64_t done = 0;
    (void)arg;
    for (;;) {
        uint64_t end = (uint64_t)g_rpt_pos - g_wav_job.start, bytes;
        uint32_t b32, riff;
        if (end > g_wav_job.cap) end = g_wav_job.cap;
        if (end - done > (uint64_t)RPT_FRAMES - XA2_SAMPLE_RATE)       /* fell a whole ring behind */
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
        Sleep(250);
    }
}

/* F9 / Back+R3. Says so on screen at once; the "saved" toast follows when
 * the files are written (a second or so). */
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
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    LARGE_INTEGER st_t[6];     /* stage times: start, wav, report ring, tap/gaplog, state, submit */
    g_mixed_now = NULL;
    if (!g_xa2_initialized || !g_xa2_source) return 0;
    QueryPerformanceCounter(&st_t[0]);

    /* RECOMP_AUDIO_WAV=<path>: record what is sent to the speakers (up to
     * ten minutes), to check the output without listening. */
    {
        static FILE *wav;
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
        static uint32_t wav_cap;   /* RECOMP_AUDIO_WAV_SECONDS (default 600) */
        if (!wav_cap) {
            const char *cs = getenv("RECOMP_AUDIO_WAV_SECONDS");
            unsigned long secs = cs && *cs ? strtoul(cs, NULL, 10) : 600ul;
            if (secs < 1 || secs > 3600) secs = 600;
            wav_cap = 48000u * 4u * (uint32_t)secs;
        }
        if (wav && g_rpt_ring) {
            /* Written from the report ring by its own thread: a disk write on
             * this thread stalled the speakers for 20-60 ms at a time in test
             * runs ([APUSLOW] output ... wav, AUDIO-REPORT-HOTKEY-20261022). */
            static int started;
            if (!started) {
                HANDLE th;
                started = 1;
                g_wav_job.f = wav; g_wav_job.start = (uint64_t)g_rpt_pos; g_wav_job.cap = wav_cap / 4u;
                th = CreateThread(NULL, 0, wav_writer_thread, &g_wav_job, 0, NULL);
                if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); }
            }
            {
                uint64_t done = (uint64_t)g_rpt_pos + (uint64_t)num_samples - g_wav_job.start;
                wav_bytes = (uint32_t)((done < g_wav_job.cap ? done : g_wav_job.cap) * 4u);
            }
        } else if (wav && wav_bytes < wav_cap) {
            uint32_t n = (uint32_t)num_samples * 4u, riff;
            fwrite(capture_mixed(samples, num_samples), 1, n, wav);
            wav_bytes += n;
            if (wav_bytes % (48000u * 4u) < n) {    /* header and flush once a second */
                riff = wav_bytes + 36;
                fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
                fseek(wav, 40, SEEK_SET); fwrite(&wav_bytes, 4, 1, wav);
                fseek(wav, 0, SEEK_END);
                fflush(wav);
            }
        }
    }

    QueryPerformanceCounter(&st_t[1]);
    if (g_rpt_ring) {        /* the capture mix (theme, voice) and the ring copy, timed for the report */
        LARGE_INTEGER a, b2;
        QueryPerformanceCounter(&a);
        rpt_write(capture_mixed(samples, num_samples), num_samples);
        QueryPerformanceCounter(&b2);
        g_rpt_cost_qpc += (uint64_t)(b2.QuadPart - a.QuadPart);
        g_rpt_cost_n++;
    }

    QueryPerformanceCounter(&st_t[2]);
    if (g_tap) {
        AcquireSRWLockExclusive(&g_tap_lock);
        if (g_tap) {
            fwrite(capture_mixed(samples, num_samples), 4, (size_t)num_samples, g_tap);
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

    QueryPerformanceCounter(&st_t[3]);
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    QueryPerformanceCounter(&st_t[4]);
    {
        int q = (int)state.BuffersQueued;
        if (!g_st_subs || q < g_st_qmin) g_st_qmin = q;
        if (!g_st_subs || q > g_st_qmax) g_st_qmax = q;
        g_st_subs++;
        if (q >= s_nbuf) g_st_drops++;
        else if (q == 0) g_st_under++;
        q *= s_buf;
        if (g_dg_qmin < 0 || q < g_dg_qmin) g_dg_qmin = q;
        if (q > g_dg_qmax) g_dg_qmax = q;
        if ((int)state.BuffersQueued >= s_nbuf) g_dg_drops++;
        else if (state.BuffersQueued == 0 && g_xa2_frames_written > 50) g_dg_under++;
    }
    /* Dropped buffers (queue full: the APU runs ahead of the speakers) and
     * underruns (queue empty: behind) are both audible clicks; count them. */
    {
        static unsigned long drops, starves, low_queue, subs;
        static DWORD last;
        DWORD now = GetTickCount();
        subs++;
        if ((int)state.BuffersQueued >= s_nbuf) drops++;
        else if (state.BuffersQueued == 0 && subs > 50) starves++;
        if (state.BuffersQueued * (unsigned)s_buf <= 1024) low_queue++;   /* 21 ms or less */
        if (now - last >= 10000) {
            fprintf(stderr, "[XA2] submits=%lu dropped=%lu underruns=%lu low_queue=%lu submit_failures=%lu queued=%u\n",
                    subs, drops, starves, low_queue, g_xa2_submit_failures,
                    (unsigned)state.BuffersQueued);
            last = now;
        }
    }
    if ((int)state.BuffersQueued >= s_nbuf) return 0;

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > s_buf) ? s_buf : num_samples;
    memcpy(g_xa2_store + (size_t)idx * s_buf * 2, samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)(g_xa2_store + (size_t)idx * s_buf * 2);

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) {
        g_xa2_submit_failures++;
        fprintf(stderr, "[XA2] SubmitSourceBuffer failed: 0x%08lX (count=%lu)\n",
                (unsigned long)hr, g_xa2_submit_failures);
        return 0;
    }

    g_xa2_next_buf = (idx + 1) % s_nbuf;
    g_xa2_frames_written++;
    g_xa2_submitted += (uint64_t)copy_samples;
    QueryPerformanceCounter(&st_t[5]);
    if ((st_t[5].QuadPart - st_t[0].QuadPart) * 1000 > 15 * rpt_qpc_freq()) {
        /* an output step over 15 ms: which part (AUDIO-REPORT-HOTKEY-20261022) */
        static unsigned n;
        double f = (double)rpt_qpc_freq() / 1000.0;
        if (n++ < 200)
            fprintf(stderr, "[APUSLOW] output %.1f ms: wav %.1f ring %.1f tap %.1f state %.1f submit %.1f\n",
                    (st_t[5].QuadPart - st_t[0].QuadPart) / f, (st_t[1].QuadPart - st_t[0].QuadPart) / f,
                    (st_t[2].QuadPart - st_t[1].QuadPart) / f, (st_t[3].QuadPart - st_t[2].QuadPart) / f,
                    (st_t[4].QuadPart - st_t[3].QuadPart) / f, (st_t[5].QuadPart - st_t[4].QuadPart) / f);
    }
    return 1;
}

int xa2_get_buffer_size(void)
{
    return s_buf;
}

#else /* !_WIN32 -- POSIX stubs (no audio output yet) */

int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
void xbox_AudioRecordTap(const char *path)          { (void)path; }
int  xa2_get_buffer_size(void)                        { return 0; }
void xa2_take_stats(int *a, int *b, unsigned *c, unsigned *d, unsigned *e) { *a = *b = 0; *c = *d = *e = 0; }
uint32_t xa2_wav_frames(void)                         { return 0; }
int  xa2_queued_samples(void)                         { return -1; }
int  xa2_legacy(void)                                 { return 1; }
int  xa2_target_samples(void)                         { return 0; }
uint64_t xa2_report_frames(void)                      { return 0; }
void xa2_diag_take(int *a, int *b, unsigned *c, unsigned *d) { *a = *b = 0; *c = *d = 0; }
void xa2_diag_push(const XA2DiagSec *r)               { (void)r; }
int  xbox_AudioReportSave(char *o, size_t n)          { (void)o; (void)n; return 0; }
void xbox_AudioReportRequest(void)                    {}

#endif /* _WIN32 */
