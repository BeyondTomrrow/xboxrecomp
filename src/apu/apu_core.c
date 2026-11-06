/*
 * MCPX APU Core - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "fpconv.h"
#ifdef _WIN32
#include "apu_env.h"     /* cached getenv: debug switches are read per voice */
#endif

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * IRQ handling (stubbed - no PCI bus in standalone)
 * ============================================================ */

/* The APU's interrupt line, as the kernel sees it (kernel_bridge.c raises
 * the title's connected ISR while it is set). The title's DirectSound ISR
 * reads NV_PAPU_ISTS and acknowledges by writing it, which comes back through
 * the register write path to update_irq and drops the line. Before this the
 * line went nowhere, so DirectSound never learned that a voice had stopped or
 * a stream packet had played -- NFL 2K5 waited forever on pregame speech
 * (2026-09-24). */
static volatile LONG g_apu_irq_line;
int mcpx_apu_irq_line(void) { return g_apu_irq_line != 0; }
/* Set by the host (kernel bridge): wakes the thread that delivers the line.
 * Polling it every 10 ms left the front end trapped 20-37 ms each time a
 * voice went idle (2026-10-01). */
void (*g_apu_irq_wake)(void);

/* How long a notifier waits for the line, and the line for the title's ISR to
 * acknowledge it ([APULOCK] report). DirectSound's Stop spins on the game
 * thread until that acknowledgement, so these are game-thread milliseconds. */
volatile int64_t g_apu_irq_note_us;      /* when the pending notifier was raised (apu_vp.c) */
static int64_t g_apu_irq_line_us;
static volatile LONGLONG g_apu_note_n, g_apu_note_sum_us, g_apu_note_max_us, g_apu_ack_n, g_apu_ack_sum_us, g_apu_ack_max_us;

/* RECOMP_APU_TRACE=<file>: QPC-stamped voice-stop events, to join with a
 * stack sampler's timeline. */
void apu_tr(char c, unsigned v)
{
    static int on = -1;
    static FILE *f;
    static unsigned n;
    LARGE_INTEGER q;
    if (on < 0) {
        const char *p = getenv("RECOMP_APU_TRACE");
        f = p ? fopen(p, "w") : NULL;
        on = f != NULL;
    }
    if (!on) return;
    QueryPerformanceCounter(&q);
    fprintf(f, "%lld %c %x\n", (long long)q.QuadPart, c, v);
    if ((++n & 255) == 0) fflush(f);
}

static void update_irq(MCPXAPUState *d)
{
    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        /* In standalone mode we don't raise a PCI IRQ; the game's kernel
         * stub will poll ISTS directly or we'll signal via a flag. */
        if (InterlockedExchange(&g_apu_irq_line, 1) == 0) {
            int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            g_apu_irq_line_us = now;
            apu_tr('L', d->regs[NV_PAPU_ISTS]);
            if (g_apu_irq_note_us) {
                int64_t w = now - g_apu_irq_note_us;
                g_apu_note_n++; g_apu_note_sum_us += w;
                if (w > g_apu_note_max_us) g_apu_note_max_us = w;
            }
            if (g_apu_irq_wake)
                g_apu_irq_wake();   /* run the title's ISR now, not at the next poll */
        }
        g_apu_irq_note_us = 0;
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        if (InterlockedExchange(&g_apu_irq_line, 0) != 0) {
            int64_t w = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - g_apu_irq_line_us;
            apu_tr('A', 0);
            g_apu_ack_n++; g_apu_ack_sum_us += w;
            if (w > g_apu_ack_max_us) g_apu_ack_max_us = w;
        }
    }
}

/* A title write (or a queued one) that trapped the front end for a stopped
 * voice raises the line at once (apu_vp.c: voice_idle_now). Other notifiers
 * keep waiting for the end of the next pipeline frame: raising those early too
 * doubled the interrupts and gained nothing (2026-11-06 A/B). The caller holds
 * d->lock. */
volatile int g_apu_raise_now;
static void apu_raise_pending_irq(MCPXAPUState *d)
{
    if (g_apu_raise_now) {
        static int nowake = -1;
        if (nowake < 0) nowake = getenv("RECOMP_APU_NO_IDLE_WAKE") != NULL;
        g_apu_raise_now = 0;
        update_irq(d);
        d->set_irq = false;
        /* The line is a level and update_irq only wakes the ISR thread on its
         * rising edge: with the line still high from an earlier notifier, this
         * trap waited 8 ms at the median for the next vblank deadline, against
         * 0.04 ms on a rising edge (2026-11-06 trace), with the title spinning. */
        if (!nowake && g_apu_irq_line && g_apu_irq_wake)
            g_apu_irq_wake();
    }
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100);
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)r);
     */
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)val);
     */
    (void)size;

    switch (addr) {
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        if (addr == NV_PAPU_FECTL) apu_tr('U', (unsigned)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        /* Starting the APU has to start the frame thread.
         *
         * The thread idles on pause_requested, which init sets and only the
         * test tone ever cleared -- so a title that enabled the APU through
         * these registers got an APU that stayed asleep. Nothing then advanced
         * the front end, and a title waiting on a notify completion (the
         * FEMEMDATA magic write, which is how completion reaches guest memory)
         * waited forever. Wreckless hangs exactly there during DirectSound
         * init, and because it initialises its whole engine behind a
         * successful DirectSound create, that hang is not confined to audio.
         *
         * Resume whenever the write is not switching the block off; the thread
         * re-checks FECTL itself and idles again if it is halted or trapped. */
        {
            uint32_t sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
            uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            bool running =
                ((sectl & NV_PAPU_SECTL_XCNTMODE) != NV_PAPU_SECTL_XCNTMODE_OFF)
                && ((fectl & NV_PAPU_FECTL_FEMETHMODE)
                    != NV_PAPU_FECTL_FEMETHMODE_HALTED);
            if (running && d->pause_requested) {
                d->pause_requested = false;
                fprintf(stderr, "[APU] started by the title"
                                " (SECTL=%08X FECTL=%08X)\n", sectl, fectl);
            }
        }
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* On Linux, waveOut* are inert stubs from win32_compat.h: the APU's
 * waveOut fallback path stays inactive and never produces audio. */

/* Ring of waveOut buffers for double-buffering */
#define WAVEOUT_NUM_BUFS 4
#define WAVEOUT_BUF_SAMPLES 2048  /* ~42.7ms at 48kHz, matches 8-frame delivery rate */
#define MIXER_FRAME_SAMPLES 256  /* Internal mixing frame size (matches frame_buf) */

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;

    /* Try XAudio2 first (lower latency) */
    if (xa2_init()) {
        fprintf(stderr, "[APU] Using XAudio2 audio backend\n");
        return;
    }
    fprintf(stderr, "[APU] XAudio2 unavailable, falling back to waveOut\n");

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

/* Game audio gain, 0..1 (xbox_AudioSetGameGain): lets a broadcast theme
 * play over a ducked game mix (src/nfl2k5_presentation.cpp). */
static volatile float g_game_gain = 1.0f;
/* Smoothed level (RMS, 0..1 of full scale) of what the game plays, so a
 * broadcast theme can dip under the announcers (xbox_AudioGameLevel). */
static volatile float g_game_level;
/* The broadcast mix acts inside the mixdown (apu_dsp.c): the gain scales the
 * game's music/crowd bed but not the commentary, and the level reported is
 * the commentary's, so a theme dips only under the announcers. */
extern volatile float g_apu_bed_gain, g_apu_voice_level;
extern volatile int g_apu_mute_music;
extern volatile float g_apu_voice_gain;
/* Every change of the host-side mix controls is logged (they are rare: a
 * theme start/stop, a fade in steps): a control left set is how the crowd
 * and effects went missing before (AUDIO-FIX-20261021). */
static void mix_log(const char *what, float was, float now)
{
    float dd = now - was;
    if (dd < 0) dd = -dd;
    if (dd >= 0.1f || ((now == 0.0f || now == 1.0f) && was != now))
        fprintf(stderr, "[MIXCTL] t=%lu %s %.2f -> %.2f\n", GetTickCount(), what, was, now);
}
void xbox_AudioSetCommentaryGain(float gain)
{
    float g = gain < 0.0f ? 0.0f : gain > 1.0f ? 1.0f : gain;
    mix_log("commentary_gain", g_apu_voice_gain, g);
    g_apu_voice_gain = g;
}
void xbox_AudioMuteGameMusic(int mute)
{
    mix_log("mute_game_music", (float)g_apu_mute_music, (float)(mute != 0));
    g_apu_mute_music = mute;
}
float xbox_AudioGameLevel(void) { return g_apu_voice_level; }
void xbox_AudioSetGameGain(float gain)
{
    float g = gain < 0.0f ? 0.0f : gain > 1.0f ? 1.0f : gain;
    mix_log("bed_gain", g_apu_bed_gain, g);
    g_apu_bed_gain = g;
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    /* XAudio2 path. monitor.frame_buf holds this frame's 256 samples: the
     * EP output mcpx_apu_dsp_frame wrote for the eight VP sub-frames (the
     * game's own mixed audio). This used to clear the buffer first and play
     * only the host-side test tone and software mixer, so every sample the
     * title's DirectSound produced was thrown away, and it submitted a
     * 1024-sample buffer per 256-sample frame (2026-09-26). Now: the APU
     * output plus the software mixer on top, gathered into XAudio2-sized
     * buffers. */
    if (xa2_is_active()) {
        static int16_t acc[1024][2];
        static int acc_n;
        int buf_size = xa2_get_buffer_size();
        int i;

        if (buf_size > 1024) buf_size = 1024;
        if (g_test_tone.active && !g_audio_muted) {
            for (i = 0; i < MIXER_FRAME_SAMPLES; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                d->monitor.frame_buf[i][0] = s;
                d->monitor.frame_buf[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }
        if (!g_audio_muted) {
            int64_t m0 = qemu_clock_get_us(QEMU_CLOCK_REALTIME), m1;
            mixer_render(d->monitor.frame_buf, MIXER_FRAME_SAMPLES);
            m1 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            if (m1 - m0 > 10000) {
                static unsigned n;
                if (n++ < 100)
                    fprintf(stderr, "[APUSLOW] software mixer %.1f ms (its lock was held)\n", (m1 - m0) / 1000.0);
            }
        } else
            memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        {
            double e = 0;
            float rms, a;
            for (i = 0; i < MIXER_FRAME_SAMPLES; i++) {
                double l = d->monitor.frame_buf[i][0], r = d->monitor.frame_buf[i][1];
                e += l * l + r * r;
            }
            rms = (float)(sqrt(e / (2.0 * MIXER_FRAME_SAMPLES)) / 32768.0);
            a = rms > g_game_level ? 0.5f : 0.02f;     /* fast attack, slow release */
            g_game_level += (rms - g_game_level) * a;
        }
        for (i = 0; i < MIXER_FRAME_SAMPLES; i++) {
            float gain = g_game_gain;
            acc[acc_n][0] = (int16_t)(d->monitor.frame_buf[i][0] * gain);
            acc[acc_n][1] = (int16_t)(d->monitor.frame_buf[i][1] * gain);
            if (++acc_n == buf_size) {
                xa2_submit_samples((const int16_t *)acc, buf_size);
                acc_n = 0;
            }
        }
        /* The next frame starts silent: the EP writes all 256 samples when
         * the pipeline runs, and an idle APU contributes nothing. */
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        return;
    }

    if (!g_waveout.initialized) return;

    int idx = g_waveout.next_buf;
    WAVEHDR *hdr = &g_waveout.hdrs[idx];

    /* Wait if this buffer is still playing (with timeout) */
    int wait_loops = 0;
    while (!(hdr->dwFlags & WHDR_DONE) && (hdr->dwFlags & WHDR_INQUEUE)) {
        qemu_mutex_unlock(&d->lock);
        Sleep(1);
        qemu_mutex_lock(&d->lock);
        if (++wait_loops > 50) break;
    }

    /* Fill the large waveOut buffer by rendering multiple 256-sample frames */
    int16_t *out = (int16_t *)g_waveout.bufs[idx];
    int remaining = WAVEOUT_BUF_SAMPLES;
    int out_offset = 0;

    while (remaining > 0) {
        int chunk = (remaining < MIXER_FRAME_SAMPLES) ? remaining : MIXER_FRAME_SAMPLES;

        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));

        /* Test tone (skip if muted) */
        if (g_test_tone.active && !g_audio_muted) {
            for (int i = 0; i < chunk; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                d->monitor.frame_buf[i][0] = s;
                d->monitor.frame_buf[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }

        /* Mix software voices (skip if muted) */
        if (!g_audio_muted)
            mixer_render(d->monitor.frame_buf, chunk);

        /* Copy to waveOut buffer */
        memcpy(out + out_offset * 2, d->monitor.frame_buf, chunk * 2 * sizeof(int16_t));
        out_offset += chunk;
        remaining -= chunk;
    }

    /* Submit to waveOut */
    hdr->dwFlags &= ~WHDR_DONE;
    waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR));

    g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
    g_waveout.frames_written++;
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

/* Paced by the speakers (AUDIO-FIX-20261021, dropouts): a monitor frame runs
 * when the XAudio2 queue holds less than the target (56 ms) and waits while it
 * holds more, so the APU follows the audio device's clock and keeps a constant
 * cushion. Pacing by the wall clock alone kept whatever cushion was left after
 * the last resync -- often 0-2 x 21 ms -- and every 20-40 ms pause of the
 * pipeline (an ISR wait, a busy host thread) emptied it: 26-34 underruns per
 * 10 minutes outside loads. After a pause the queue is refilled at once (the
 * frames run back to back, as the old 64 ms catch-up did). When the device
 * stops consuming (unplugged, lost), the wall clock takes over again. RECOMP_AUDIO_LEGACY=1: the old pacing. */
static int throttle_by_queue(MCPXAPUState *d)
{
    int target = xa2_target_samples();
    int64_t t0 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    static int64_t last_exit, stall_at;
    extern int64_t g_apu_stall_total_us;
    if (!xa2_is_active() || target <= 0 || xa2_legacy())
        return 0;
    if (last_exit && t0 - last_exit > 25000) {
        /* 256 samples of work took over 25 ms of wall time: say how full the
         * queue still is (an ISR wait is logged as such elsewhere). */
        static unsigned n;
        int q = xa2_queued_samples();
        if (n++ < 200)
            fprintf(stderr, "[APUSLOW] t=%lu %.1f ms since the last output frame (ISR wait %.1f ms), queue %d ms\n",
                    GetTickCount(), (t0 - last_exit) / 1000.0, (g_apu_stall_total_us - stall_at) / 1000.0,
                    q < 0 ? -1 : q / 48);
    }
    while (!d->pause_requested) {
        int q = xa2_queued_samples();
        int ms;
        if (q < 0) return 0;              /* not consuming: wall clock */
        if (q < target) break;
        ms = (q - target) / 48;           /* time until it drops to the target */
        if (ms < 1) ms = 1;
        if (ms > 20) ms = 20;
        qemu_cond_timedwait(&d->cond, &d->lock, ms);
    }
    d->next_frame_time_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    d->sleep_acc_us += (int)(d->next_frame_time_us - t0);
    last_exit = d->next_frame_time_us;
    stall_at = g_apu_stall_total_us;
    return 1;
}

static void throttle(MCPXAPUState *d)
{
    if (d->ep_frame_div % 8) {
        return;
    }
    if (throttle_by_queue(d)) {
        apu_tr('B', 0);
        return;
    }

    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    if (d->next_frame_time_us == 0 ||
        now_us - d->next_frame_time_us > 64000) {
        /* Up to 64 ms behind (a front-end trap, a scheduling hiccup) is
         * caught up by running frames back to back; the speaker queue has
         * the room. Further behind, resynchronise. */
        d->next_frame_time_us = now_us;
    }

    while (!d->pause_requested) {
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t remaining_ms = (d->next_frame_time_us - now_us) / 1000;
        if (remaining_ms > 0) {
            qemu_cond_timedwait(&d->cond, &d->lock, (int)remaining_ms);
        } else {
            break;
        }
    }
    d->next_frame_time_us += EP_FRAME_US;

    d->sleep_acc_us += (int)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - now_us);
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    {
        /* A pipeline frame that takes over 20 ms (it should take well under
         * one) is logged with where the time went: an audio-thread stall
         * is what empties the speaker queue (AUDIO-REPORT-HOTKEY-20261022). */
        int64_t t0 = qemu_clock_get_us(QEMU_CLOCK_REALTIME), t1, t2, t3;
        mcpx_apu_vp_frame(d, mixbins);
        t1 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        mcpx_apu_dsp_frame(d, mixbins);
        t2 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        mcpx_apu_monitor_frame(d);
        t3 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        if (t3 - t0 > 20000) {
            static unsigned n;
            if (n++ < 200)
                fprintf(stderr, "[APUSLOW] t=%lu frame %.1f ms: vp %.1f dsp %.1f monitor/output %.1f\n",
                        GetTickCount(), (t3 - t0) / 1000.0, (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, (t3 - t2) / 1000.0);
        }
    }

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

int64_t g_apu_stall_us;          /* time the pipeline waited for the title's ISR (RECOMP_APU_PITCH_LOG) */
unsigned g_apu_stall_n;
/* The same for the audio report (always on), and the VP frames emitted silent
 * because an ISR wait passed 40 ms (each one 32 samples of game audio lost). */
int64_t g_apu_dg_stall_us;
unsigned g_apu_dg_stall_n, g_apu_dg_silent_vp;
unsigned long g_apu_silent_vp_total;   /* the same, never reset (RECOMP_APU_PITCH_LOG) */
int64_t g_apu_stall_total_us;          /* ISR waits in all (the [APUSLOW] lines) */

static void apu_let_writers_in(MCPXAPUState *d);

static void *mcpx_apu_frame_thread(void *arg)
{
#if defined(__linux__)
    prctl(PR_SET_NAME, "apu_frame", 0, 0, 0);   /* named in the Android [CPU] report */
#endif
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    /* Above the game's and the renderer's threads: the pipeline is short work
     * between waits, and a normal-priority audio thread lost 100-300 ms to busy
     * host threads during frame hitches (VP frame deficits with no ISR wait,
     * AUDIO-FIX-20261021 dropouts). RECOMP_APU_PRIORITY=0 leaves it normal. */
    if (!xa2_legacy() && (!getenv("RECOMP_APU_PRIORITY") || strcmp(getenv("RECOMP_APU_PRIORITY"), "0") != 0))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    qemu_mutex_lock(&d->lock);

    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        /* Always run the audio output loop — the software mixer and test tone
         * need continuous frame delivery regardless of APU register state.
         * The VP/DSP pipeline (se_frame) only runs when registers allow it. */
        /* As xemu: while the front end is trapped or halted the pipeline
         * stands still until the title's ISR releases it, and the time is
         * made up afterwards (throttle). Emitting silent frames meanwhile cut
         * 20-37 ms holes in all audio whenever a voice ended -- choppy menu
         * sounds, chopped commentary, effects "like a fan" (2026-10-01).
         * Stalls longer than 40 ms fall back to silent frames so output and
         * the software mixer never stop. */
        {
            static int64_t stall_since;
            int xm = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]), NV_PAPU_SECTL_XCNTMODE);
            uint32_t fc = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            /* Stall statistics, every 5 s: count, total and longest wait for
             * the title's ISR, and how many ran into the 40 ms fallback. */
            static unsigned st_n, st_timeouts;
            static int64_t st_total, st_max, st_last_log;
            static int st_timed_out;
            if (xm != NV_PAPU_SECTL_XCNTMODE_OFF && !g_test_tone.active &&
                (fc & (NV_PAPU_FECTL_FEMETHMODE_TRAPPED | NV_PAPU_FECTL_FEMETHMODE_HALTED))) {
                int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
                if (!stall_since) { stall_since = now; st_timed_out = 0; }
                if (now - stall_since < 40000) {
                    update_irq(d);
                    d->set_irq = false;
                    qemu_cond_timedwait(&d->cond, &d->lock, 1);
                    continue;
                }
                if (!st_timed_out) { st_timed_out = 1; st_timeouts++; }
            } else if (stall_since) {
                int64_t dur = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - stall_since;
                st_n++;
                st_total += dur;
                g_apu_stall_us += dur;
                g_apu_stall_n++;
                g_apu_dg_stall_us += dur;
                g_apu_stall_total_us += dur;
                g_apu_dg_stall_n++;
                if (dur > st_max) st_max = dur;
                stall_since = 0;
            }
            {
                int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
                if (now_ms - st_last_log > 5000) {
                    static int on = -1;
                    if (on < 0) on = getenv("RECOMP_APU_VOICE_LOG") != NULL;
                    if (on && (st_n || st_timeouts))
                        fprintf(stderr, "[APU] ISR stalls: %u in 5 s, total %.1f ms, max %.1f ms, >40 ms %u\n",
                                st_n, st_total / 1000.0, st_max / 1000.0, st_timeouts);
                    st_n = st_timeouts = 0;
                    st_total = st_max = 0;
                    st_last_log = now_ms;
                }
            }
        }

        throttle(d);

        /* The doorbell ack stands in for the GP DSP, which on hardware runs
         * whatever the front end is doing. Tying it to se_frame stopped it
         * whenever FECTL was trapped or halted, and DirectSound then waits
         * forever to post its next command: Burnout 3 stalls in
         * sub_002F805E polling the same doorbell it was acked on at init. */
        mcpx_apu_dsp_ack_poll(d);

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        bool apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED);

        if (apu_active && !g_test_tone.active) {
            /* Full pipeline: VP voices → DSP → monitor → waveOut */
            se_frame(d);
            /* As xemu's frame loop: a frame that raised a notifier or trapped
             * the front end updates the interrupt line. */
            if (d->set_irq) {
                update_irq(d);
                d->set_irq = false;
            }
        } else {
            /* A trapped or halted front end is waiting for the title's ISR
             * (xemu sets set_irq here). */
            if (fectl & (NV_PAPU_FECTL_FEMETHMODE_TRAPPED | NV_PAPU_FECTL_FEMETHMODE_HALTED)) {
                update_irq(d);
                d->set_irq = false;
            }
            /* Lightweight: just monitor frame (test tone + software mixer) */
            if (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF &&
                (fectl & (NV_PAPU_FECTL_FEMETHMODE_TRAPPED | NV_PAPU_FECTL_FEMETHMODE_HALTED)))
            { g_apu_dg_silent_vp++; g_apu_silent_vp_total++; }
            mcpx_apu_monitor_frame(d);
            d->ep_frame_div++;
        }
        apu_let_writers_in(d);
    }

    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    if (d->gp.dsp) {
        memset((void *)d->gp.dsp->core.pram_opcache, 0,
               sizeof(d->gp.dsp->core.pram_opcache));
    }
    if (d->ep.dsp) {
        memset((void *)d->ep.dsp->core.pram_opcache, 0,
               sizeof(d->ep.dsp->core.pram_opcache));
    }
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Init DSP (GP/EP - stubbed) */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Init monitor (waveOut output) */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    mcpx_apu_wait_for_idle(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    qemu_mutex_lock(&d->lock);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    qemu_thread_join(&d->apu_thread);
    mcpx_apu_vp_finalize(d);
    mcpx_apu_monitor_finalize(d);

    free(d);
    g_state = NULL;
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    }
    /* GP (0x30000) and EP (0x50000) regions ignored for now */
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

static void apu_q_flush_for_read(MCPXAPUState *d);

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        apu_q_flush_for_read(d);   /* queued voice commands first (mcpx_apu_mmio_write) */
        return mcpx_apu_read(d, (hwaddr)addr, size);
    }
    return 0;
}

/* Voice commands (the VP method FIFO, 0x20000-0x2FFFF) from the title's
 * threads. The frame thread processes voices holding d->lock for bursts of
 * up to 8 pipeline frames, so a write used to wait for the rest of the burst:
 * in SOFTDRINK play DirectSound sends 20-50 thousand of them a second, the
 * game thread spent up to a third of its time waiting for the lock and frames
 * ran late (2026-11-02). Now a writer that finds the lock taken queues the
 * command and goes on; whoever holds the lock next runs the queue first --
 * the frame thread between frames, or the next writer. Commands still never
 * land mid-frame and still run in order. Main-register writes (interrupts,
 * control) stay synchronous, and run the queue first. RECOMP_APU_SYNC_WRITES=1
 * makes every write wait for the lock again. */
typedef struct { uint32_t addr, val, size; } ApuQueued;
#define APU_QUEUE_LEN 16384u
static ApuQueued g_apu_q[APU_QUEUE_LEN];
static volatile LONG g_apu_q_head, g_apu_q_tail;    /* head: next to run; tail: next free */
static CRITICAL_SECTION g_apu_q_cs;
static volatile LONG g_apu_q_init;
static volatile LONGLONG g_apu_mmio_wait_us, g_apu_mmio_writes, g_apu_mmio_queued, g_apu_mmio_slow, g_apu_mmio_full, g_apu_read_flush_us;

static void apu_q_init(void)
{
    if (g_apu_q_init == 2) return;
    if (InterlockedCompareExchange(&g_apu_q_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_apu_q_cs);
        InterlockedExchange(&g_apu_q_init, 2);
    }
    while (g_apu_q_init != 2) SwitchToThread();
}

/* Run the queued commands. The caller holds d->lock. */
static void apu_q_drain(MCPXAPUState *d)
{
    ApuQueued batch[256];
    if (g_apu_q_init != 2 || g_apu_q_head == g_apu_q_tail) return;
    for (;;) {
        unsigned n = 0;
        EnterCriticalSection(&g_apu_q_cs);
        while (n < 256 && g_apu_q_head != g_apu_q_tail) {
            batch[n++] = g_apu_q[g_apu_q_head];
            g_apu_q_head = (g_apu_q_head + 1) & (APU_QUEUE_LEN - 1);
        }
        LeaveCriticalSection(&g_apu_q_cs);
        if (!n) break;
        for (unsigned i = 0; i < n; i++)
            mcpx_apu_dispatch_mmio(d, (hwaddr)batch[i].addr, batch[i].val, batch[i].size, true);
    }
    apu_raise_pending_irq(d);
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    static int sync = -1;
    LARGE_INTEGER a, b, f;
    if (!d) return;
    if (sync < 0) sync = getenv("RECOMP_APU_SYNC_WRITES") != NULL;
    apu_q_init();
    InterlockedAdd64(&g_apu_mmio_writes, 1);
    if (!sync && addr >= 0x20000 && addr < 0x30000) {
        if (TryEnterCriticalSection(&d->lock.cs)) {
            apu_q_drain(d);
            mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
            apu_raise_pending_irq(d);
            qemu_mutex_unlock(&d->lock);
            return;
        }
        EnterCriticalSection(&g_apu_q_cs);
        if (((g_apu_q_tail + 1) & (APU_QUEUE_LEN - 1)) != g_apu_q_head) {
            ApuQueued *e = &g_apu_q[g_apu_q_tail];
            e->addr = (uint32_t)addr; e->val = (uint32_t)val; e->size = size;
            g_apu_q_tail = (g_apu_q_tail + 1) & (APU_QUEUE_LEN - 1);
            LeaveCriticalSection(&g_apu_q_cs);
            InterlockedAdd64(&g_apu_mmio_queued, 1);
            return;
        }
        LeaveCriticalSection(&g_apu_q_cs);   /* full: wait for the lock below */
        InterlockedAdd64(&g_apu_mmio_full, 1);
    }
    /* Serialise with the frame thread, which processes voices holding
     * d->lock (as xemu does). Unlocked, the title's voice commands (set
     * position, start, pause/resume, buffer pointers) landed mid-frame and
     * were overwritten by the voice processor: NFL 2K5's music streamer saw
     * its positions jump and restarted the song every 1-5 s, and announcer
     * lines were cut (2026-10-01). The lock is a recursive critical section. */
    QueryPerformanceCounter(&a);
    qemu_mutex_lock(&d->lock);
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    {
        LONGLONG us = (b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart;
        InterlockedAdd64(&g_apu_mmio_wait_us, us);
        if (us > 1000) InterlockedAdd64(&g_apu_mmio_slow, 1);
    }
    apu_q_drain(d);
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
    apu_raise_pending_irq(d);
    qemu_mutex_unlock(&d->lock);
}

/* Frame thread, holding d->lock between pipeline frames. */
static void apu_let_writers_in(MCPXAPUState *d)
{
    apu_q_drain(d);
}

/* A main-register read after queued voice commands sees their effects. */
static void apu_q_flush_for_read(MCPXAPUState *d)
{
    LARGE_INTEGER a, b, f;
    if (g_apu_q_init != 2 || g_apu_q_head == g_apu_q_tail) return;
    QueryPerformanceCounter(&a);
    qemu_mutex_lock(&d->lock);
    apu_q_drain(d);
    qemu_mutex_unlock(&d->lock);
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    InterlockedAdd64(&g_apu_read_flush_us, (b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart);
}

void mcpx_apu_lock_report(void)
{
    static LONGLONG w0, n0, s0, q0, f0, r0;
    LONGLONG w = g_apu_mmio_wait_us, n = g_apu_mmio_writes, s = g_apu_mmio_slow, q = g_apu_mmio_queued;
    LONGLONG fu = g_apu_mmio_full, r = g_apu_read_flush_us;
    fprintf(stderr, "[APULOCK] %lld register writes (%lld queued, %lld with the queue full), waited %.1f ms in all "
            "for the APU lock (%lld over 1 ms), reads waited %.1f ms\n",
            (long long)(n - n0), (long long)(q - q0), (long long)(fu - f0), (w - w0) / 1000.0,
            (long long)(s - s0), (r - r0) / 1000.0);
    w0 = w; n0 = n; s0 = s; q0 = q; f0 = fu; r0 = r;
    {
        LONGLONG nn = g_apu_note_n, ns = g_apu_note_sum_us, an = g_apu_ack_n, as = g_apu_ack_sum_us;
        static LONGLONG nn0, ns0, an0, as0;
        fprintf(stderr, "[APUIRQ] %lld notifiers waited %.2f ms on average for the line (longest %.1f); "
                "%lld acknowledgements took %.2f ms on average (longest %.1f)\n",
                (long long)(nn - nn0), nn > nn0 ? (ns - ns0) / 1000.0 / (double)(nn - nn0) : 0.0, g_apu_note_max_us / 1000.0,
                (long long)(an - an0), an > an0 ? (as - as0) / 1000.0 / (double)(an - an0) : 0.0, g_apu_ack_max_us / 1000.0);
        nn0 = nn; ns0 = ns; an0 = an; as0 = as;
        g_apu_note_max_us = g_apu_ack_max_us = 0;
    }
}

/* ============================================================
 * APU Test Tone - Direct waveOut sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that waveOut
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Make sure waveOut is running - enable SECTL and resume APU thread */
    qemu_mutex_lock(&d->lock);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers to waveOut
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    apu_mixer_stop(slot);
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

int apu_mixer_set_position(int slot, uint32_t byte_offset)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return 0;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    uint32_t frame_bytes = v->num_channels * sizeof(int16_t);
    int valid = (v->num_channels == 1 || v->num_channels == 2) &&
                byte_offset < v->pcm_bytes &&
                byte_offset / frame_bytes < v->pcm_bytes / frame_bytes;
    if (valid) v->play_offset = ((uint64_t)(byte_offset / frame_bytes)) << 16;
    LeaveCriticalSection(&g_mixer_cs);
    return valid;
}

void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping)
{
    if (byte_offset) *byte_offset = 0;
    if (active) *active = 0;
    if (looping) *looping = 0;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (byte_offset) *byte_offset = (uint32_t)(v->play_offset >> 16) *
                                    v->num_channels * sizeof(int16_t);
    if (active) *active = v->active;
    if (looping) *looping = v->active && v->looping;
    LeaveCriticalSection(&g_mixer_cs);
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || (v->num_channels != 1 && v->num_channels != 2) ||
        v->pcm_bytes < v->num_channels * sizeof(int16_t)) {
        LeaveCriticalSection(&g_mixer_cs);
        return;
    }
    v->looping = looping;
    if (!v->active) InterlockedIncrement((volatile LONG *)&g_mixer_active_count);
    v->active = 1;

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
    LeaveCriticalSection(&g_mixer_cs);

    /* The frame thread takes the APU lock before the mixer lock. */
    extern MCPXAPUState *g_state;
    if (g_state) {
        qemu_mutex_lock(&g_state->lock);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
    LeaveCriticalSection(&g_mixer_cs);
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * Keep 16 fractional bits in a wide offset so buffers can exceed 65536 frames. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;
    EnterCriticalSection(&g_mixer_cs);

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint64_t inc = ((uint64_t)voice->sample_rate << 16) / 48000;
        uint64_t pos = voice->play_offset;
        uint64_t end = (uint64_t)total_frames << 16;
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos %= end;
                    src_frame = (uint32_t)(pos >> 16);
                } else {
                    pos = 0;
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = pos % end;
            } else if (voice->active) {
                voice->play_offset = 0;
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
}
