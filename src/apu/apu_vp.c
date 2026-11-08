/*
 * MCPX APU Voice Processor - Standalone extraction from xemu
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

#ifdef _MSC_VER
#define RECOMP_APU_TLS __declspec(thread)
#else
#define RECOMP_APU_TLS _Thread_local
#endif
#include "apu_state.h"
#include "fpconv.h"
#include "apu_xaudio2.h"   /* audio report records */
#ifdef _WIN32
#include "apu_env.h"     /* cached getenv: debug switches are read per voice */
#endif

/* #define DEBUG_MCPX */

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

/* ============================================================
 * Voice list register table
 * ============================================================ */

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, /* 2D */
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, /* 3D */
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, /* MP */
};

/* ============================================================
 * Notify status helper
 * ============================================================ */

/* RECOMP_APU_SSL_LOG=1: stream packet bookkeeping -- each segment list the
 * title hands a voice (SSL A/B) and each "done" the voice reports. */
static int ssl_log_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_APU_SSL_LOG") != NULL;
    return on;
}

static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    hwaddr notify_offset = d->regs[NV_PAPU_FENADDR];
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15;

    stb_phys(address_space_memory, notify_offset, (uint8_t)status);
    stb_phys(address_space_memory, notify_offset - 1, 1);
    if (ssl_log_on() && (notifier == MCPX_HW_NOTIFIER_SSLA_DONE || notifier == MCPX_HW_NOTIFIER_SSLA_DONE + 1))
        fprintf(stderr, "[SSL] %03X done %c t=%lu\n", v, 'A' + notifier - MCPX_HW_NOTIFIER_SSLA_DONE, GetTickCount());

    qatomic_or(&d->regs[NV_PAPU_ISTS],
               NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    {
        extern volatile int64_t g_apu_irq_note_us;   /* apu_core.c: the [APUIRQ] report */
        if (!d->set_irq) g_apu_irq_note_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    }
    d->set_irq = true;
}

/* ============================================================
 * Filter helpers
 * ============================================================ */

typedef struct {
    float rmin, rmax, efmin, efmax;
    int pmin, pmax, ps;
    unsigned frames, sets;
    uint8_t c_bin;            /* routed to the centre (commentary) bin */
    float gmax, gsum;         /* loudest bin gain (volume x envelope), max and sum over frames */
} PitchStat;
static PitchStat s_pstat[MCPX_HW_MAX_VOICES];
/* Called on each pitch change (voice, old, new pitch) when set by the title. */
void (*g_apu_pitch_hook)(unsigned voice, int was, int now);
/* Always on, for the audio report (AUDIO-REPORT-HOTKEY-20261022): per voice
 * and second, the rate range and the loudest volume / envelope -- a few
 * compares per voice frame; turned into one record a second (diag_report). */
typedef struct { float rmin, rmax, emax; uint16_t vmin; uint16_t frames; } DiagVoice;
static DiagVoice s_dvoice[MCPX_HW_MAX_VOICES];
static int diag_on(void)          /* RECOMP_AUDIO_NO_REPORT=1 turns the report off entirely */
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_AUDIO_NO_REPORT") == NULL;
    return on;
}
static int pitch_log_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_APU_PITCH_LOG") != NULL;
    return on;
}
static void voice_resampler_reset(uint16_t v);
static void voice_resampler_stream_ready(uint16_t v);
static uint8_t s_adpcm_cbo_valid[MCPX_HW_MAX_VOICES];   /* defined with voice_get_samples */
static uint8_t s_adpcm_shift[MCPX_HW_MAX_VOICES];
/* Gain each voice last applied to each of its 8 bins (volume ramps). */
static float s_bin_gain[MCPX_HW_MAX_VOICES][8];
static uint8_t s_bin_gain_bin[MCPX_HW_MAX_VOICES][8];
static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    memset(s_bin_gain[v], 0, sizeof(s_bin_gain[v]));       /* a new sound fades in over one frame */
    memset(&d->vp.filters[v].svf, 0, sizeof(d->vp.filters[v].svf));
    hrtf_filter_clear_history(&d->vp.filters[v].hrtf);
    voice_resampler_reset(v);
    s_adpcm_cbo_valid[v] = 0;
    s_adpcm_shift[v] = 0;
    if (d->vp.filters[v].resampler) {
        src_reset(d->vp.filters[v].resampler);
    }
}

static bool voice_should_mute(uint16_t v)
{
    bool m = (g_dbg_voice_monitor >= 0) && (v != g_dbg_voice_monitor);
    return m || mcpx_apu_debug_is_muted(v);
}

/* ============================================================
 * Utility functions
 * ============================================================ */

static float clampf(float v, float mn, float mx)
{
    if (v < mn) return mn;
    if (v > mx) return mx;
    return v;
}

static float attenuate(uint16_t vol)
{
    vol &= 0xFFF;
    return (vol == 0xFFF) ? 0.0f : powf(10.0f, vol / (64.0f * -20.0f));
}

/* ============================================================
 * Voice register accessors (read/write voice struct in RAM)
 * ============================================================ */

static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               hwaddr offset, uint32_t mask)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(address_space_memory, voice + offset) & mask) >>
           ctz32(mask);
}

static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           hwaddr offset, uint32_t mask, uint32_t val)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    uint32_t v = ldl_le_phys(address_space_memory, voice + offset) & ~mask;
    stl_le_phys(address_space_memory, voice + offset,
                v | ((val << ctz32(mask)) & mask));
}

/* ============================================================
 * Voice off / lock
 * ============================================================ */

void apu_tr(char c, unsigned v);          /* apu_core.c: RECOMP_APU_TRACE */

static void voice_off(MCPXAPUState *d, uint16_t v)
{
    apu_tr('V', v);
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument);

/* The title stopped a voice: tell it the voice is idle now rather than when the
 * next pipeline frame walks the voice list. DirectSound's Stop spins on the game
 * thread until that trap has been serviced (sub_0003CAF0). The console's VP
 * passes every 0.67 ms, but the frame thread runs 8 frames in a burst and sleeps
 * 4-10 ms between bursts, so each stopped voice cost that long, one voice after
 * another: 105 of the 107 frames over 33 ms in a sampled game were inside this
 * spin (2026-11-06). Same call the frame makes; only for a voice on a list, as
 * the frame only finds those, and not over a trap still being serviced (the
 * title reads which voice from FEDECPARAM); the frame then does it as before.
 * The caller holds d->lock, between frames. RECOMP_APU_LATE_IDLE=1: the frame only. */
static void voice_idle_now(MCPXAPUState *d, uint16_t v)
{
    extern volatile int g_apu_raise_now;     /* apu_core.c */
    static int late = -1;
    if (late < 0) late = getenv("RECOMP_APU_LATE_IDLE") != NULL;
    if (late || (d->regs[NV_PAPU_FECTL] & (NV_PAPU_FECTL_FEMETHMODE_TRAPPED | NV_PAPU_FECTL_FEMETHMODE_HALTED)))
        return;
    for (int list = 0; list < 3; list++) {
        uint16_t c = (uint16_t)d->regs[voice_list_regs[list].top];
        for (int i = 0; c != 0xFFFF && i < MCPX_HW_MAX_VOICES; i++) {
            if (c == v) {
                fe_method(d, SE2FE_IDLE_VOICE, v);
                if (d->set_irq) g_apu_raise_now = 1;
                return;
            }
            c = (uint16_t)voice_get_mask(d, c, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                         NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        }
    }
}

static void voice_lock(MCPXAPUState *d, uint16_t v, bool lock)
{
    assert(v < MCPX_HW_MAX_VOICES);
    qemu_mutex_lock(&d->lock);

    uint64_t mask = 1ULL << (v % 64);
    if (lock) {
        d->vp.voice_locked[v / 64] |= mask;
    } else {
        d->vp.voice_locked[v / 64] &= ~mask;
    }

    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    uint64_t mask = 1ULL << (v % 64);
    return (qatomic_read(&d->vp.voice_locked[v / 64]) & mask) != 0;
}

/* ============================================================
 * HRIR coefficient setter
 * ============================================================ */

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               int8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] = int8_to_float(value);
}

/* ============================================================
 * Front-End method dispatch
 * ============================================================ */

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;

    d->regs[NV_PAPU_FEDECMETH] = method;
    d->regs[NV_PAPU_FEDECPARAM] = argument;
    unsigned int selected_handle, list;
    {
        /* What the title asks of the voice processor: the first few methods,
         * then a running count (2026-09-24, NFL 2K5 speech never finishing). */
        static unsigned long n;
        n++;
        static unsigned long ons, offs;
        if (method == NV1BA0_PIO_VOICE_ON && ++ons <= 30)
            fprintf(stderr, "[APU] VOICE_ON #%lu arg %08X (method #%lu)\n", ons, argument, n);
        if (method == NV1BA0_PIO_VOICE_OFF && ++offs <= 30)
            fprintf(stderr, "[APU] VOICE_OFF #%lu arg %08X (method #%lu)\n", offs, argument, n);
        if (n <= 40 || (n % 5000) == 0)
            fprintf(stderr, "[APU] method #%lu %04X arg %08X (voice on %lu off %lu)\n",
                    n, method, argument, ons, offs);
    }

    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, (uint16_t)d->regs[NV_PAPU_FECV], argument & 1);
        break;

    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;

    case NV1BA0_PIO_VOICE_ON: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(
                d, (uint16_t)antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           next_handle);
            voice_set_mask(d, (uint16_t)antecedent_voice,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           selected_handle);
        }

        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0;
        d->vp.ssl[selected_handle].ssl_index = 0;

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVF, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }

        voice_reset_filters(d, (uint16_t)selected_handle);
        if (getenv("RECOMP_APU_PITCH_STACK") && selected_handle < 0x100) {
            /* the start of each sound, to tell a new sound from a pitch change (RECOMP_APU_PITCH_STACK) */
            LARGE_INTEGER qf, qc;
            QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
            fprintf(stderr, "[PITCHON] qpc=%.1f %03X pitch %d\n", (double)qc.QuadPart * 1000.0 / (double)qf.QuadPart,
                    (unsigned)selected_handle,
                    (int16_t)voice_get_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_TAR_PITCH_LINK, NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH));
        }
        if (getenv("RECOMP_APU_VOICE_LOG"))
            fprintf(stderr, "[VOICE] %03X ON ba %08X ebo %06X lbo %06X fmt %08X pitch %d list %d (t=%lu)\n",
                    (unsigned)selected_handle,
                    voice_get_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA),
                    voice_get_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO),
                    voice_get_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_PSH_SAMPLE, NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO),
                    voice_get_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFFu),
                    (int16_t)voice_get_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_TAR_PITCH_LINK, NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH),
                    (int)list, GetTickCount());
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        apu_tr('R', selected_handle);

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        uint16_t rr;
        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_OFF:
        apu_tr('O', argument & NV1BA0_PIO_VOICE_OFF_HANDLE);
        if (getenv("RECOMP_APU_VOICE_LOG"))
            fprintf(stderr, "[VOICE] %03X OFF at cbo %06X (t=%lu)\n", argument & NV1BA0_PIO_VOICE_OFF_HANDLE,
                    voice_get_mask(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE), NV_PAVS_VOICE_PAR_OFFSET,
                                   NV_PAVS_VOICE_PAR_OFFSET_CBO), GetTickCount());
        voice_off(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE));
        voice_idle_now(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE));
        break;

    case NV1BA0_PIO_VOICE_PAUSE:
        if (getenv("RECOMP_APU_VOICE_LOG")) {
            fprintf(stderr, "[VOICE] %03X %s at cbo %06X (t=%lu)\n", argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE,
                    (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) ? "pause" : "resume",
                    voice_get_mask(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE), NV_PAVS_VOICE_PAR_OFFSET,
                                   NV_PAVS_VOICE_PAR_OFFSET_CBO), GetTickCount());
            if (getenv("RECOMP_APU_PAUSE_STACK")) {
                /* Guest return addresses on the calling thread's stack: who
                 * pauses/resumes the voice (the FE method runs on the guest
                 * thread that wrote the MMIO register). */
                #if defined(_MSC_VER)
                    extern RECOMP_APU_TLS uint32_t g_esp;
                #else
                    extern __thread uint32_t g_esp;
                #endif
                extern ptrdiff_t xbox_GetMemoryOffset(void);
                const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
                int shown = 0;
                fprintf(stderr, "[VOICE]   stack:");
                for (uint32_t k = 0; k < 96 && shown < 14; k++) {
                    uint32_t w = *(const uint32_t *)(gm + g_esp + k * 4);
                    if (w >= 0x00011000u && w < 0x00480000u) { fprintf(stderr, " %08X", w); shown++; }
                }
                fprintf(stderr, "\n");
            }
        }
        voice_set_mask(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE),
                       NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;

    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
        d->vp.hrtf.current_entry =
            GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        break;

    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;

    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV0, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV1, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVF, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF, argument);
        break;

    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        int current_voice = d->regs[NV_PAPU_FECV];
        voice_set_mask(d, (uint16_t)current_voice,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        if (current_voice < MCPX_HW_MAX_3D_VOICES &&
            handle != HRTF_NULL_HANDLE) {
            assert(handle < HRTF_ENTRY_COUNT);
            hrtf_filter_set_target_params(&d->vp.filters[current_voice].hrtf,
                                          d->vp.hrtf.entries[handle].hrir,
                                          d->vp.hrtf.entries[handle].itd);
        }
        break;
    }

    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_LFO_ENV, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        if (pitch_log_on()) s_pstat[(uint16_t)d->regs[NV_PAPU_FECV] & 0xFF].sets++;
        if (g_apu_pitch_hook) {   /* the title's probe (src/main.c NFL2K5_CROWD_PROBE) */
            uint16_t cv = (uint16_t)d->regs[NV_PAPU_FECV];
            int16_t was = (int16_t)voice_get_mask(d, cv, NV_PAVS_VOICE_TAR_PITCH_LINK, NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
            int16_t now = (int16_t)((argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
            if (was != now) g_apu_pitch_hook(cv, was, now);
        }
        if (getenv("RECOMP_APU_PITCH_STACK")) {
            /* Who changes a voice's pitch: the voice, old -> new, and the
             * guest return addresses on the writing thread's stack (as
             * RECOMP_APU_PAUSE_STACK). =3d: the 3D voices (0-0x3F) only.
             * At most 6000 lines. */
            static int shown, only3d = -1;
            uint16_t cv = (uint16_t)d->regs[NV_PAPU_FECV];
            int16_t was = (int16_t)voice_get_mask(d, cv, NV_PAVS_VOICE_TAR_PITCH_LINK, NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
            int16_t now = (int16_t)((argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
            if (only3d < 0) only3d = !strcmp(getenv("RECOMP_APU_PITCH_STACK"), "3d");
            if (was != now && shown < 6000 && (!only3d || cv < MCPX_HW_MAX_3D_VOICES)) {
                extern RECOMP_APU_TLS uint32_t g_esp;
                extern ptrdiff_t xbox_GetMemoryOffset(void);
                const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
                int k2 = 0;
                shown++;
                LARGE_INTEGER qf, qc;
                QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
                fprintf(stderr, "[PITCHSET] qpc=%.1f %03X %d -> %d cbo %06X stack:",
                        (double)qc.QuadPart * 1000.0 / (double)qf.QuadPart, cv, was, now,
                        voice_get_mask(d, cv, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO));
                for (uint32_t k = 0; k < 96 && k2 < 12; k++) {
                    uint32_t w = *(const uint32_t *)(gm + g_esp + k * 4);
                    if (w >= 0x00011000u && w < 0x00480000u) { fprintf(stderr, " %08X", w); k2++; }
                }
                fprintf(stderr, "\n");
            }
        }
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        /* A new base address is a seek/rebind, not a volume update.  Discard
         * queued source samples and ADPCM alignment from the previous buffer
         * so a restarted ambience or commentary stream cannot play stale
         * audio before the new data. */
        voice_resampler_reset((uint16_t)d->regs[NV_PAPU_FECV]);
        s_adpcm_cbo_valid[(uint16_t)d->regs[NV_PAPU_FECV]] = 0;
        s_adpcm_shift[(uint16_t)d->regs[NV_PAPU_FECV]] = 0;
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                        NV_PAVS_VOICE_CUR_PSL_START,
                        NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        if (getenv("RECOMP_APU_VOICE_LOG"))
            fprintf(stderr, "[VOICE] %03X set lbo %06X cbo %06X (t=%lu)\n", (unsigned)d->regs[NV_PAPU_FECV], argument,
                    voice_get_mask(d, (uint16_t)d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO),
                    GetTickCount());
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        if (getenv("RECOMP_APU_VOICE_LOG"))
            fprintf(stderr, "[VOICE] %03X set cbo %06X (was %06X, t=%lu)\n", (unsigned)d->regs[NV_PAPU_FECV], argument,
                    voice_get_mask(d, (uint16_t)d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO),
                    GetTickCount());
        /* CBO is explicitly moved by DirectSound when a voice is recycled or
         * resumed.  The resampler owns a separate FIFO, so it must follow the
         * guest cursor rather than replaying prefetched samples. */
        voice_resampler_reset((uint16_t)d->regs[NV_PAPU_FECV]);
        s_adpcm_cbo_valid[(uint16_t)d->regs[NV_PAPU_FECV]] = 0;
        s_adpcm_shift[(uint16_t)d->regs[NV_PAPU_FECV]] = 0;
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                        NV_PAVS_VOICE_PAR_OFFSET,
                        NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        if (getenv("RECOMP_APU_VOICE_LOG"))
            fprintf(stderr, "[VOICE] %03X set ebo %06X cbo %06X (t=%lu)\n", (unsigned)d->regs[NV_PAPU_FECV], argument,
                    voice_get_mask(d, (uint16_t)d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO),
                    GetTickCount());
        if (argument == 0xFFFFFFFFu) {
            /* A zero-length play region (EBO = start + 0 - 1): the title is
             * about to play a sound it never loaded, and the voice then reads
             * RAM from its base address for minutes (xemu never writes this;
             * docs/AUDIO-USER-REPORTS-20261022.md). Log who does it, with
             * the guest return addresses on the calling thread's stack. */
            static int shown;
            if (shown < 24) {
                extern RECOMP_APU_TLS uint32_t g_esp;
                extern ptrdiff_t xbox_GetMemoryOffset(void);
                const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
                int n = 0;
                shown++;
                fprintf(stderr, "[VOICE] %03X EBO=-1 (zero-length region) ba %06X lbo %06X stack:", (unsigned)d->regs[NV_PAPU_FECV],
                        voice_get_mask(d, (uint16_t)d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA),
                        voice_get_mask(d, (uint16_t)d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CUR_PSH_SAMPLE, NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO));
                for (uint32_t k = 0; k < 128 && n < 16; k++) {
                    uint32_t w = *(const uint32_t *)(gm + g_esp + k * 4);
                    if (w >= 0x00011000u && w < 0x00480000u) { fprintf(stderr, " %08X", w); n++; }
                }
                fprintf(stderr, "\n");
            }
        }
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.outbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        int ssl = 0;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        if (ssl_log_on())
            fprintf(stderr, "[SSL] %03X set A base %u count %u (playing %c seg %d) t=%lu\n", current_voice,
                    (unsigned)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE),
                    (unsigned)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT),
                    'A' + d->vp.ssl[current_voice].ssl_index, d->vp.ssl[current_voice].ssl_seg, GetTickCount());
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        /* Persistent voices use SSL A/B as a producer queue.  An empty
         * packet is temporary silence, not end-of-stream; wake a resampler
         * that reached that temporary gap as soon as the title submits more
         * descriptors. */
        if (d->vp.ssl[current_voice].count[ssl])
            voice_resampler_stream_ready((uint16_t)current_voice);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        int ssl = 1;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        if (ssl_log_on())
            fprintf(stderr, "[SSL] %03X set B base %u count %u (playing %c seg %d) t=%lu\n", current_voice,
                    (unsigned)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE),
                    (unsigned)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT),
                    'A' + d->vp.ssl[current_voice].ssl_index, d->vp.ssl[current_voice].ssl_seg, GetTickCount());
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        if (d->vp.ssl[current_voice].count[ssl])
            voice_resampler_stream_ready((uint16_t)current_voice);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }

    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (uint8_t)((argument >> 0) & 0x1f);
        d->vp.hrtf_submix[1] = (uint8_t)((argument >> 8) & 0x1f);
        d->vp.hrtf_submix[2] = (uint8_t)((argument >> 16) & 0x1f);
        d->vp.hrtf_submix[3] = (uint8_t)((argument >> 24) & 0x1f);
        break;

    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        d->vp.hrtf_headroom = (uint8_t)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT);
        fprintf(stderr, "[APU] HRTF headroom %u\n", d->vp.hrtf_headroom);
        break;

    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            apu_tr('I', argument);
            d->set_irq = true;
        }
        break;

    default:
        /* Handle range-based cases that can't use case ranges in MSVC */
        if (method >= NV1BA0_PIO_SET_HRIR && method < NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
            int8_t left0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
            int8_t right0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
            int8_t left1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
            int8_t right1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
            int coeff_idx = slot * 2;
            set_hrir_coeff_tar(d, 0, coeff_idx, left0);
            set_hrir_coeff_tar(d, 1, coeff_idx, right0);
            set_hrir_coeff_tar(d, 0, coeff_idx + 1, left1);
            set_hrir_coeff_tar(d, 1, coeff_idx + 1, right1);
        } else if (method == NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            int8_t left30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
            int8_t right30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
            int16_t itd = (int16_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
            set_hrir_coeff_tar(d, 0, 30, left30);
            set_hrir_coeff_tar(d, 1, 30, right30);
            d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
        } else if (method >= NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET &&
                   method < NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH + 8 * 64) {
            assert((method & 0x3) == 0);
            hwaddr addr = d->regs[NV_PAPU_VPSSLADDR]
                          + (d->vp.ssl_base_page * 8)
                          + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
            stl_le_phys(address_space_memory, addr, argument);
        } else if (method >= NV1BA0_PIO_SET_SUBMIX_HEADROOM &&
                   method <= NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * (NUM_MIXBINS - 1)) {
            assert((method & 3) == 0);
            slot = (method - NV1BA0_PIO_SET_SUBMIX_HEADROOM) / 4;
            d->vp.submix_headroom[slot] =
                (uint8_t)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT);
            fprintf(stderr, "[APU] submix %u headroom %u\n", slot, d->vp.submix_headroom[slot]);
        } else if ((method >= NV1BA0_PIO_SET_OUTBUF_BA &&
                    method < NV1BA0_PIO_SET_OUTBUF_BA + 32) ||
                   (method >= NV1BA0_PIO_SET_OUTBUF_LEN &&
                    method < NV1BA0_PIO_SET_OUTBUF_LEN + 32)) {
            /* Outbuf base/length - ignore for now */
        } else {
            /* Unknown method - silently ignore */
            DPRINTF("Unknown FE method: 0x%08X arg=0x%08X\n", method, argument);
        }
        break;
    }
}

/* ============================================================
 * VP MMIO read/write (exposed to apu_core.c)
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;

    switch (addr) {
    case NV1BA0_PIO_FREE:
        return 0x80; /* Always pretend queue is empty */
    default:
        break;
    }
    return 0;
}

void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    (void)size;

    /* Dispatch known methods through fe_method */
    fe_method(d, (uint32_t)addr, (uint32_t)val);
}

/* ============================================================
 * SGE data pointer resolution
 * ============================================================ */

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(address_space_memory, sge_base + entry * 4 * 2);
    return prd_address + addr % TARGET_PAGE_SIZE;
}

/* ============================================================
 * Envelope processing
 * ============================================================ */

static float voice_step_envelope(MCPXAPUState *d, uint16_t v, uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = (uint8_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        return 1.0f;

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0x00);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 0.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t attack_rate =
            (uint16_t)voice_get_mask(d, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);
        float value;
        if (attack_rate == 0) {
            value = 255.0f;
        } else {
            if (count <= (uint32_t)(attack_rate * 16)) {
                value = (count * 0xFF) / (float)(attack_rate * 16);
            } else {
                value = 255.0f;
            }
        }
        voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        if (count == (uint32_t)(attack_rate * 16)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            count = hold_time * 16;
        } else {
            count++;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t decay_rate =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
            count = decay_rate * 16;
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 1.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t decay_rate =
            (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        float value;
        if (decay_rate == 0) {
            value = 0.0f;
        } else {
            value = 255.0f * powf(0.99988799f, (decay_rate * 16 - count) *
                                                   4096.0f / decay_rate);
        }
        if (value <= (sustain_level + 0.2f) || (value > 255.0f)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
            voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN: {
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0x00);
        voice_set_mask(d, v, lvl_reg, lvl_mask, sustain_level);
        return sustain_level / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t release_rate = (uint16_t)voice_get_mask(d, v, rr_reg, rr_mask);
        if (release_rate == 0) count = 0;
        float value = 0;
        if (count == 0) {
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, ++cur);
        } else {
            float pos = clampf(1 - count / (release_rate * 16.0f), 0, 1);
            uint8_t lvl = (uint8_t)voice_get_mask(d, v, lvl_reg, lvl_mask);
            value = powf((float)M_E, -6.91f * pos) * lvl;
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT) {
            voice_off(d, v);
        }
        return 0.0f;

    default:
        fprintf(stderr, "[APU] Unknown envelope state 0x%x\n", cur);
        return 0.0f;
    }
}

/* ============================================================
 * Sample fetching from voice buffers
 * ============================================================ */

/* ADPCM play position as the title sees it. DirectSound turns CBO into a
 * byte offset and NFL 2K5's music streamer writes the next chunk there; the
 * voice processor decodes whole 36-byte / 64-sample blocks, so on the
 * console that offset lands on a block. Stepping CBO sample by sample left
 * it mid-block, the streamer copied data 14-22 bytes off the block grid, and
 * the menu music decoded as static (2026-09-26). The register shows the
 * block; the sample within it is kept here while the register still holds
 * the value we wrote (the title can move it at any time). */
static uint32_t s_adpcm_cbo_reg[MCPX_HW_MAX_VOICES], s_adpcm_cbo_sub[MCPX_HW_MAX_VOICES];
static uint8_t s_adpcm_cbo_valid[MCPX_HW_MAX_VOICES];

/* ADPCM buffers whose data sits off the 36-byte block grid.
 *
 * NFL 2K5 streams its menu music into a looping ADPCM buffer per channel.
 * In this port the game restarts that stream more often than on the console
 * (timing), and each restart writes the song from the current write cursor,
 * which is usually mid-block: the whole buffer then holds valid ADPCM at a
 * constant byte shift (8, 22, ... measured 2026-09-26), and decoding it on
 * the grid gives static. When a block header is invalid (step index > 88 or
 * reserved byte != 0), find the shift at which the next 8 headers are all
 * valid and decode at that shift until the grid itself is valid again. A
 * random 8-block match is ~1e-23, so this never fires on good data. */
static uint8_t s_adpcm_shift[MCPX_HW_MAX_VOICES];

volatile int g_apu_mute_music;   /* xbox_AudioMuteGameMusic */

static int adpcm_header_ok(uint32_t w)
{
    return ((w >> 16) & 0xFF) <= 88 && (w >> 24) == 0;
}

static uint32_t adpcm_word_at(MCPXAPUState *d, uint32_t ba, uint32_t buf_bytes, uint32_t off)
{
    return ldl_le_phys(address_space_memory,
                       get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF, ba + off % buf_bytes));
}

static void adpcm_realign(MCPXAPUState *d, uint32_t v, uint32_t ba, uint32_t buf_bytes, uint32_t block_index)
{
    uint32_t base = block_index * 36u, sh, k;
    uint32_t old = s_adpcm_shift[v], best = old, best_score = 0;

    /* A single random payload word has about a 1/736 chance of looking like
     * an ADPCM header.  Accepting the current shift after one such word made
     * the decoder stay on the wrong grid for a whole block whenever the
     * title rewrote part of its circular music buffer.  Score a short local
     * run instead.  Requiring the current block to be valid still lets us
     * switch exactly at a boundary between differently aligned regions. */
    for (sh = 0; sh < 36; sh++) {
        uint32_t score = 0;
        for (k = 0; k < 4; k++) {
            if (!adpcm_header_ok(adpcm_word_at(d, ba, buf_bytes, base + k * 36u + sh)))
                break;
        }
        score = k;
        if (score > best_score || (score == best_score && sh == old)) {
            best = sh;
            best_score = score;
        }
    }
    if (best_score) {
        if (best != old && getenv("RECOMP_APU_VOICE_LOG"))
            fprintf(stderr, "[VOICE] %03X ADPCM realigned: shift %u -> %u at block %u (score %u/4)\n",
                    v, old, best, block_index, best_score);
        s_adpcm_shift[v] = (uint8_t)best;
    }
}

static int voice_get_samples(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    unsigned int container_size_index = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    bool paused = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
    bool loop = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                               NV_PAVS_VOICE_CFG_FMT_LOOP) != 0;
    uint32_t ebo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST) != 0;

    int ssl_index = 0, ssl_seg = 0, page = 0, count = 0;
    int seg_len = 0, seg_cs = 0, seg_spb = 0, seg_s = 0;
    hwaddr segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    uint32_t adpcm_block[36 * 2 / 4];
    int16_t adpcm_decoded[65 * 2];

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) return -1;

    if (stream) {
        if (!persist) {
            int eacur = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                voice_off(d, (uint16_t)v);
                return -1;
            }
        }

        assert(!loop);
        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        if (count == 0) {
            voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                voice_off(d, (uint16_t)v);
            } else {
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE +
                                  d->vp.ssl[v].ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        hwaddr addr = d->regs[NV_PAPU_VPSSLADDR] + page * 8;
        segment_offset = ldl_le_phys(address_space_memory, addr);
        segment_length = ldl_le_phys(address_space_memory, addr + 4);
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }
        assert(seg_len > 0);
        ebo = seg_len - 1;
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);
    if (adpcm && s_adpcm_cbo_valid[v] && cbo == s_adpcm_cbo_reg[v])
        cbo += s_adpcm_cbo_sub[v];
    bool end_pad = !stream && loop && lbo <= ebo && lbo + ADPCM_SAMPLES_PER_BLOCK > ebo;

    if (adpcm) {
        block_size = 36;
    } else {
        block_size = container_size;
    }
    block_size *= samples_per_block;

    int sample_count = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != (int)block_index) {
                uint32_t linear_addr = block_index * (uint32_t)block_size;
                if (!stream && samples_per_block == 1) {
                    uint32_t buf_bytes = (ebo / ADPCM_SAMPLES_PER_BLOCK + 1) * 36u;
                    adpcm_realign(d, v, ba, buf_bytes, block_index);
                    linear_addr = (linear_addr + s_adpcm_shift[v]) % buf_bytes;
                }
                if (stream) {
                    hwaddr addr = segment_offset + linear_addr;
                    memcpy(adpcm_block, apu_phys(addr),
                           block_size);
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            ldl_le_phys(address_space_memory, addr);
                        linear_addr += 4;
                    }
                }
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            hwaddr addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                uint32_t linear_addr = ba + cbo * (uint32_t)block_size;
                addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                    linear_addr);
            }

            for (unsigned int channel = 0; channel < channels; channel++) {
                uint32_t ival;
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = ldub_phys(address_space_memory, addr);
                    fval = uint8_to_float((uint8_t)(ival & 0xff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    ival = lduw_le_phys(address_space_memory, addr);
                    fval = int16_to_float((int16_t)(ival & 0xffff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int24_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int32_to_float(ival);
                    break;
                default:
                    fval = 0.0f;
                    break;
                }
                samples[sample_count][channel] = fval;
                addr += container_size;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }
        /* The one-block end loop of a one-shot (LOOP, LBO = EBO - 63) is
         * DirectSound's end padding: it holds the voice until the title
         * stops it. NFL 2K5's 3D sound engine re-binds such voices and
         * restores them to LBO, so some never stop, and an audible last
         * block repeated forever was the in-game "train horn" drone
         * (2026-10-02). Play the padding as silence. */
        if (end_pad && cbo >= lbo) {
            samples[sample_count][0] = 0.0f;
            samples[sample_count][1] = 0.0f;
        }
    }

    if (stream && getenv("RECOMP_APU_VOICE_LOG")) {
        /* Stream voices: the segment being played and the loudest sample
         * read from it, every 2 s -- silent data and a wrong address look
         * the same from the speakers. */
        static DWORD slog[MCPX_HW_MAX_VOICES];
        static float speak[MCPX_HW_MAX_VOICES];
        for (int i = 0; i < sample_count; i++) {
            float a = fabsf(samples[i][0]);
            if (a > speak[v]) speak[v] = a;
        }
        if (GetTickCount() - slog[v] > 2000) {
            slog[v] = GetTickCount();
            fprintf(stderr, "[STREAM] %03X ssl %d/%d seg %d/%d off %08X len %u cs %d ssize %d spb %d stereo %d peak %.4f first %08X\n",
                    v, ssl_index, d->vp.ssl[v].count[ssl_index], ssl_seg, count, (uint32_t)segment_offset,
                    (unsigned)seg_len, seg_cs, sample_size, seg_spb, stereo, speak[v],
                    segment_offset ? ldl_le_phys(address_space_memory, segment_offset) : 0);
            speak[v] = 0;
        }
    }

    if (cbo >= ebo) {
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                /* Move to next segment */
            } else {
                int next_index = (ssl_index + 1) % 2;
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                voice_off(d, (uint16_t)v);
            }
        }
    }

    /* One-shot sounds loop a single padding block at the end of the buffer
     * (LOOP set, LBO = EBO - 63); DirectSound stops the voice once CBO is
     * past LBO. Reporting a block-aligned CBO there kept it at exactly LBO,
     * so effects were never stopped and replayed their last block forever --
     * the in-game "train horn" drone (2026-10-02). The block grid is only
     * reported for the long looping buffers the music streamer writes into. */
    if (adpcm && !(loop && lbo + ADPCM_SAMPLES_PER_BLOCK > ebo)) {
        uint32_t sub = cbo % ADPCM_SAMPLES_PER_BLOCK;
        s_adpcm_cbo_sub[v] = sub;
        s_adpcm_cbo_reg[v] = cbo - sub;
        s_adpcm_cbo_valid[v] = 1;
        cbo -= sub;
    } else {
        s_adpcm_cbo_valid[v] = 0;
    }
    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    return sample_count;
}

/* ============================================================
 * Voice resampling (simplified - no libsamplerate)
 *
 * Since libsamplerate is stubbed, we do a simple nearest-neighbor
 * resample. This gives us functional audio at the cost of quality.
 * ============================================================ */

/* Per-voice resampler state. The VP plays each voice at its pitch
 * (rate = output/source = 2^(-pitch/4096)); this used to be ignored and
 * every voice was fetched 1:1 at 48 kHz, so 22/32/44.1 kHz material played
 * at the wrong speed with hard steps between frames -- heard as static
 * (2026-09-26). Cubic (Catmull-Rom) interpolation over a 4-sample history
 * that persists across frames; source samples are pulled from the voice in
 * small chunks into a FIFO. */
#define RS_FIFO 256
typedef struct {
    float hist[4][2];
    double pos;
    float fifo[RS_FIFO][2];
    int fifo_n, fifo_i;
    bool ended;
    /* band-limited (windowed-sinc) history for upsampled voices: the
     * output point lies between sh[RS_TAPS/2-1] and sh[RS_TAPS/2] */
    float sh[32][2];
    int stereo_left;          /* > 0 while L != R was seen within the window */
} VoiceResampler;

/* Windowed-sinc interpolation for upsampled voices (source rate < 48 kHz).
 * xemu's VP resamples each voice with libsamplerate's sinc converter: its
 * output has nothing above the sources' Nyquist (a cliff at ~10-11 kHz in a
 * match, where the fastest voices are 22 kHz); the cubic interpolator left
 * images up to 20 kHz (16 dB more 11-20 kHz energy than xemu after a 4th-order
 * low-pass, 39 dB before; docs/AUDIO-USER-REPORTS-20261022.md). 32 taps,
 * Kaiser beta 8 (~80 dB stopband), cutoff 0.42 of the source rate (pass to
 * ~0.34, stop from 0.50 = the source Nyquist), 512 phases with linear
 * interpolation between them, each phase normalised to unity gain at DC.
 * RECOMP_APU_CUBIC=1: the old cubic interpolator without any filter. */
#define RS_TAPS 32
#define RS_PHASES 512
static float s_sinc_tab[RS_PHASES + 1][RS_TAPS];
static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; k++) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
    return sum;
}
static void sinc_table_init(void)
{
    const double fc = 0.42, beta = 8.0, half = RS_TAPS / 2.0;
    for (int p = 0; p <= RS_PHASES; p++) {
        double t = (double)p / RS_PHASES, sum = 0.0;
        for (int k = 0; k < RS_TAPS; k++) {
            double x = (k - (RS_TAPS / 2 - 1)) - t;        /* distance from the output point */
            double sx = 2.0 * fc * x;
            double sinc = fabs(sx) < 1e-9 ? 1.0 : sin(3.14159265358979 * sx) / (3.14159265358979 * sx);
            double u = x / half;
            double win = fabs(u) >= 1.0 ? 0.0 : bessel_i0(beta * sqrt(1.0 - u * u)) / bessel_i0(beta);
            s_sinc_tab[p][k] = (float)(2.0 * fc * sinc * win);
            sum += s_sinc_tab[p][k];
        }
        for (int k = 0; k < RS_TAPS; k++) s_sinc_tab[p][k] = (float)(s_sinc_tab[p][k] / sum);
    }
}
static VoiceResampler s_rs[MCPX_HW_MAX_VOICES];

static void voice_resampler_reset(uint16_t v)
{
    memset(&s_rs[v], 0, sizeof(s_rs[v]));
    s_rs[v].pos = 1.0;
}

/* Unlike a one-shot voice, a persistent SSL voice is allowed to run out of
 * descriptors while its producer refills A or B.  voice_resample records that
 * temporary starvation in ended so it does not spin reading an empty list.
 * The next SSL submission must clear it; VOICE_ON is not sent for every crowd
 * or ambience packet.  Keep the interpolation history for a smooth restart,
 * but discard the empty FIFO state. */
static void voice_resampler_stream_ready(uint16_t v)
{
    VoiceResampler *r = &s_rs[v];
    if (r->ended) {
        r->ended = false;
        r->fifo_i = 0;
        r->fifo_n = 0;
    }
}

static int voice_resample(MCPXAPUState *d, uint16_t v, float samples[][2],
                          int requested_num, float rate)
{
    VoiceResampler *r = &s_rs[v];
    double step = (rate > 0.0f) ? 1.0 / (double)rate : 1.0;
    if (step > 64.0) step = 64.0;
    if (step < 1.0 / 64.0) step = 1.0 / 64.0;
    static int cubic = -1;
    if (cubic < 0) { cubic = getenv("RECOMP_APU_CUBIC") != NULL; if (!cubic) sinc_table_init(); }
    /* upsampled voices only; 48 kHz (helper voices) and faster sources keep
     * the cubic path. The sinc history is filled as the voice plays, so a
     * switch between the paths mid-voice is harmless. */
    bool use_sinc = !cubic && rate > 1.0001f;

    for (int i = 0; i < requested_num; i++) {
        while (r->pos >= 1.0) {
            float in[2] = { 0.0f, 0.0f };
            r->pos -= 1.0;
            if (r->fifo_i >= r->fifo_n && !r->ended) {
                int need = (int)((requested_num - i) * step) + 4;
                if (need > RS_FIFO) need = RS_FIFO;
                r->fifo_n = r->fifo_i = 0;
                memset(r->fifo, 0, sizeof(r->fifo[0]) * need);
                if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                    r->ended = true;
                } else {
                    int got = 0;
                    while (got < need) {
                        int c = voice_get_samples(d, v, &r->fifo[got], need - got);
                        if (c <= 0) break;
                        got += c;
                        if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                            NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
                            break;
                    }
                    if (got == 0) r->ended = true;
                    r->fifo_n = got;
                }
            }
            if (r->fifo_i < r->fifo_n) {
                in[0] = r->fifo[r->fifo_i][0];
                in[1] = r->fifo[r->fifo_i][1];
                r->fifo_i++;
            }
            memmove(r->hist[0], r->hist[1], sizeof(r->hist[0]) * 3);
            r->hist[3][0] = in[0];
            r->hist[3][1] = in[1];
            if (use_sinc) {
                memmove(r->sh[0], r->sh[1], sizeof(r->sh[0]) * (RS_TAPS - 1));
                r->sh[RS_TAPS - 1][0] = in[0];
                r->sh[RS_TAPS - 1][1] = in[1];
                if (in[0] != in[1]) r->stereo_left = RS_TAPS;
                else if (r->stereo_left > 0) r->stereo_left--;
            }
        }
        if (use_sinc) {
            float fp = (float)r->pos * RS_PHASES;
            int ph = (int)fp;
            float fr = fp - (float)ph, a = 0.0f, b = 0.0f;
            if (ph >= RS_PHASES) { ph = RS_PHASES - 1; fr = 1.0f; }
            const float *k0 = s_sinc_tab[ph], *k1 = s_sinc_tab[ph + 1];
            if (r->stereo_left > 0) {
                for (int k = 0; k < RS_TAPS; k++) {
                    float h = k0[k] + (k1[k] - k0[k]) * fr;
                    a += h * r->sh[k][0];
                    b += h * r->sh[k][1];
                }
            } else {
                for (int k = 0; k < RS_TAPS; k++) a += (k0[k] + (k1[k] - k0[k]) * fr) * r->sh[k][0];
                b = a;
            }
            samples[i][0] = a;
            samples[i][1] = b;
            r->pos += step;
            continue;
        }
        float t = (float)r->pos, t2 = t * t, t3 = t2 * t;
        for (int c = 0; c < 2; c++) {
            float p0 = r->hist[0][c], p1 = r->hist[1][c];
            float p2 = r->hist[2][c], p3 = r->hist[3][c];
            samples[i][c] = 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                                    (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                                    (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
        }
        r->pos += step;
    }
    return requested_num;
}

/* RECOMP_APU_PITCH_LOG=1: once a second, every voice that played -- its
 * pitch register range, envelope pitch scale and filter-envelope range, the
 * resulting rate range, and how many pitch writes the title made -- plus the
 * pipeline's frame count and the speaker queue (AUDIO-FIX-20261021). */
static void pitch_stat_note(uint16_t v, int p, int ps, float ef, float rate)
{
    PitchStat *s = &s_pstat[v];
    if (!s->frames) {
        s->rmin = s->rmax = rate; s->pmin = s->pmax = p; s->efmin = s->efmax = ef;
    } else {
        if (rate < s->rmin) s->rmin = rate;
        if (rate > s->rmax) s->rmax = rate;
        if (p < s->pmin) s->pmin = p;
        if (p > s->pmax) s->pmax = p;
        if (ef < s->efmin) s->efmin = ef;
        if (ef > s->efmax) s->efmax = ef;
    }
    s->ps = ps;
    s->frames++;
}
/* Once a second (APU thread): one record for the audio report ring. */
int64_t g_apu_dg_lockwait_us;
unsigned g_apu_dg_lockwait_n;
static void diag_report(void)
{
    static DWORD last;
    static unsigned long frames_at;
    static LONG perf_frames_at;
    extern unsigned long g_apu_vp_frames;
    extern int64_t g_apu_dg_stall_us;
    extern unsigned g_apu_dg_stall_n, g_apu_dg_silent_vp;
    extern volatile LONG g_xbox_perf_frames;      /* nv2a_gpu_perf.inc.c */
    extern volatile LONG g_xbox_perf_worst_us;
    DWORD now = GetTickCount();
    XA2DiagSec r;
    unsigned under = 0, drops = 0;
    int v, n = 0;
    if (!diag_on()) return;
    if (!last) { last = now; frames_at = g_apu_vp_frames; perf_frames_at = g_xbox_perf_frames; return; }
    if (now - last < 1000) return;
    memset(&r, 0, sizeof r);
    r.tick = now;
    r.frames_out = xa2_report_frames();
    {
        LONG pf = g_xbox_perf_frames;
        r.fps = (float)(pf - perf_frames_at) * 1000.0f / (float)(now - last);
        perf_frames_at = pf;
        r.worst_ms = (float)InterlockedExchange(&g_xbox_perf_worst_us, 0) / 1000.0f;
    }
    r.vpframes = (uint32_t)(g_apu_vp_frames - frames_at);
    xa2_diag_take(&r.qmin, &r.qmax, &under, &drops);
    r.underruns = under; r.drops = drops;
    r.silent_vp = g_apu_dg_silent_vp; g_apu_dg_silent_vp = 0;
    r.stalls = g_apu_dg_stall_n; g_apu_dg_stall_n = 0;
    r.stall_ms = (float)(g_apu_dg_stall_us / 1000.0); g_apu_dg_stall_us = 0;
    r.lockwaits = g_apu_dg_lockwait_n; g_apu_dg_lockwait_n = 0;
    r.lockwait_ms = (float)(g_apu_dg_lockwait_us / 1000.0); g_apu_dg_lockwait_us = 0;
    for (v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        DiagVoice *dv = &s_dvoice[v];
        if (!dv->frames) continue;
        r.active++;
        if (dv->rmax > dv->rmin * 1.0006f) {          /* moved by more than a cent */
            float g = attenuate(dv->vmin) * dv->emax, ratio = dv->rmax / dv->rmin;
            int k = n;
            if (n == XA2_DIAG_VOICES) {                /* full: replace the smallest move */
                int m = 0;
                for (k = 1; k < n; k++)
                    if (r.chg[k].rmax / r.chg[k].rmin < r.chg[m].rmax / r.chg[m].rmin) m = k;
                if (r.chg[m].rmax / r.chg[m].rmin >= ratio) { memset(dv, 0, sizeof *dv); continue; }
                k = m;
            } else n++;
            /* rate = 2^(-pitch/4096): the voice plays at 48000 / rate Hz */
            r.chg[k].v = (uint16_t)v;
            r.chg[k].rmin = 48000.0f / dv->rmax;
            r.chg[k].rmax = 48000.0f / dv->rmin;
            r.chg[k].gain = g;
        }
        memset(dv, 0, sizeof *dv);
    }
    r.nchg = (uint16_t)n;
    xa2_diag_push(&r);
    {   /* RECOMP_APU_VP_DUMP=<file>: once a second, the whole VP voice table
         * (256 x 128 bytes, NV_PAVS_VOICE_*) after an 8-byte header (frames
         * in the RECOMP_AUDIO_WAV recording, tick) -- the same bytes a
         * pmemsave of VPVADDR gives in xemu, so tools/audio/vp_decode.py
         * reads both sides (docs/AUDIO-USER-REPORTS-20261022.md). */
        static FILE *vpf; static int vp_on = -1;
        if (vp_on < 0) { const char *p = getenv("RECOMP_APU_VP_DUMP"); vp_on = p != NULL; if (p) vpf = fopen(p, "wb"); }
        if (vpf && g_state) {
            extern uint32_t xa2_wav_frames(void);
            uint32_t h[2] = { xa2_wav_frames(), (uint32_t)now };
            fwrite(h, 4, 2, vpf);
            fwrite(apu_phys(g_state->regs[NV_PAPU_VPVADDR]), 1, 256 * NV_PAVS_SIZE, vpf);
            fflush(vpf);
        }
    }
    last = now;
    frames_at = g_apu_vp_frames;
}

static void pitch_stat_report(MCPXAPUState *d)
{
    static DWORD last;
    static unsigned long frames_at;
    extern unsigned long g_apu_vp_frames;
    DWORD now = GetTickCount();
    if (!pitch_log_on()) return;
    if (!last) { last = now; frames_at = g_apu_vp_frames; return; }
    if (now - last < 1000) return;
    {
        extern volatile float g_apu_bed_gain, g_apu_voice_gain;
        extern volatile int g_apu_mute_music;
        extern uint32_t xa2_wav_frames(void);
        extern int64_t g_apu_lock_wait_us;
        extern unsigned g_apu_lock_wait_n;
        extern int64_t g_apu_stall_us;
        extern unsigned g_apu_stall_n;
        extern void xa2_take_stats(int *qmin, int *qmax, unsigned *drops, unsigned *under, unsigned *subs);
        int qmin = 0, qmax = 0;
        unsigned drops = 0, under = 0, subs = 0;
        xa2_take_stats(&qmin, &qmax, &drops, &under, &subs);
        LARGE_INTEGER qf, qc;
        QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
        extern unsigned long g_apu_silent_vp_total;
        static ULONGLONG cpu_at;          /* this (APU) thread's CPU time, 100 ns */
        ULONGLONG cpu_now;
        FILETIME tc, te, tk, tu;
#ifdef _WIN32
        GetThreadTimes(GetCurrentThread(), &tc, &te, &tk, &tu);
#else
        memset(&tk, 0, sizeof tk); memset(&tu, 0, sizeof tu); (void)tc; (void)te;
#endif
        cpu_now = ((ULONGLONG)tk.dwHighDateTime << 32 | tk.dwLowDateTime) + ((ULONGLONG)tu.dwHighDateTime << 32 | tu.dwLowDateTime);
        fprintf(stderr, "[PITCH] qpc=%.0f t=%lu ms=%lu vpframes=%lu queue=%d..%d submits=%u dropped=%u underruns=%u bed=%.2f comm=%.2f mute_music=%d wav=%u lockwaits=%u lockwait_ms=%.1f stalls=%u stall_ms=%.1f silentvp=%lu apu_cpu_ms=%.1f\n",
                (double)qc.QuadPart * 1000.0 / (double)qf.QuadPart,
                (unsigned long)now, (unsigned long)(now - last), g_apu_vp_frames - frames_at,
                qmin, qmax, subs, drops, under, g_apu_bed_gain, g_apu_voice_gain, g_apu_mute_music, xa2_wav_frames(),
                g_apu_lock_wait_n, g_apu_lock_wait_us / 1000.0, g_apu_stall_n, g_apu_stall_us / 1000.0, g_apu_silent_vp_total,
                cpu_at ? (cpu_now - cpu_at) / 10000.0 : 0.0);
        cpu_at = cpu_now;
        g_apu_stall_n = 0;
        g_apu_stall_us = 0;
        g_apu_lock_wait_n = 0;
        g_apu_lock_wait_us = 0;
    }
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        PitchStat *s = &s_pstat[v];
        if (!s->frames && !s->sets) continue;
        if (s->frames)
            fprintf(stderr, "[PITCH]   %03X n=%u p=%d..%d ps=%d ef=%.3f..%.3f rate=%.4f..%.4f sets=%u gain=%.3f..%.3f%s\n",
                    v, s->frames, s->pmin, s->pmax, s->ps, s->efmin, s->efmax, s->rmin, s->rmax, s->sets,
                    s->frames ? s->gsum / s->frames : 0.0f, s->gmax, s->c_bin ? " C" : "");
        memset(s, 0, sizeof *s);
    }
    (void)d;
    last = now;
    frames_at = g_apu_vp_frames;
}

/* ============================================================
 * Voice processing (main per-voice function)
 * ============================================================ */

static void voice_process(MCPXAPUState *d,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;

    struct McpxApuDebugVoice *dbg = &g_dbg.vp.v[v];
    dbg->active = true;
    dbg->stereo = stereo;
    dbg->paused = paused;

    if (paused) return;

    /* A voice with a zero-length play region (EBO written as -1, LBO
     * garbage): the title started a sound it never loaded, and the voice
     * would read RAM from its base address as ADPCM for minutes at gain
     * 0.03-0.7 -- broadband garbage over the whole mix. xemu never has such
     * a voice (0 in 61 voice-table dumps of a demo match); ours had 6 in a
     * 4-minute demo. Play nothing until the title re-binds the voice.
     * RECOMP_APU_PLAY_EMPTY=1 plays them as before (for comparison). */
    if (voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO) == 0xFFFFFFu) {
        static int play = -1;
        static uint8_t said[MCPX_HW_MAX_VOICES];
        if (play < 0) play = getenv("RECOMP_APU_PLAY_EMPTY") != NULL;
        if (!play) {
            if (!said[v]) { said[v] = 1; fprintf(stderr, "[APU] voice %03X: zero-length region (EBO -1), muted\n", v); }
            return;
        }
    }

    /* Step filter envelope */
    float ef_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    if (ef_value < 0.0f) ef_value = 0.0f;
    if (ef_value > 1.0f) ef_value = 1.0f;

    int16_t p = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                         NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0,
                                        NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float rate = 1.0f / powf(2.0f, (p + ps * 32 * ef_value) / 4096.0f);
    dbg->rate = rate;
    if (pitch_log_on()) pitch_stat_note(v, p, ps, ef_value, rate);
    if (diag_on()) {
        DiagVoice *dv = &s_dvoice[v];
        if (!dv->frames) { dv->rmin = dv->rmax = rate; dv->vmin = 0xFFF; dv->emax = 0.0f; }
        else if (rate < dv->rmin) dv->rmin = rate;
        else if (rate > dv->rmax) dv->rmax = rate;
        if (dv->frames < 0xFFFF) dv->frames++;
    }
    {
        /* RECOMP_APU_VOICE_LOG=1: each voice's pitch, rate and format, once
         * per voice start and every 5 s. */
        static int on = -1;
        static DWORD last[MCPX_HW_MAX_VOICES];
        static unsigned calls[MCPX_HW_MAX_VOICES];
        extern unsigned long g_apu_vp_frames;
        static unsigned long frames_at[MCPX_HW_MAX_VOICES];
        if (on < 0) on = getenv("RECOMP_APU_VOICE_LOG") != NULL;
        calls[v]++;
        if (on && GetTickCount() - last[v] > 5000) {
            uint32_t fmt = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFFu);
            last[v] = GetTickCount();
            fprintf(stderr, "[VOICE] %03X processed %u in %lu frames; pitch %d ps %d rate %.4f fmt %08X stream %u stereo %u ssize %u csize %u spb %u cbo %06X ebo %06X lbo %06X\n",
                    v, calls[v], g_apu_vp_frames - frames_at[v], p, ps, rate, fmt, (fmt >> 24) & 1, (fmt >> 27) & 1, (fmt >> 28) & 3, (fmt >> 30) & 3,
                    ((fmt >> 16) & 0x1F) + 1,
                    voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO),
                    voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO),
                    voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSH_SAMPLE, NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO));
            {
                /* Where the voice's samples live: base, first SGE page
                 * addresses, and the contiguous high-water mark apu_phys
                 * uses to pick low memory vs the physical window. */
                extern uint32_t xbox_ContiguousAllocatedBytes(void);
                uint32_t ba = voice_get_mask(d, v, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA);
                uint32_t sge = d->regs[NV_PAPU_VPSGEADDR];
                fprintf(stderr, "[VOICE] %03X ba %08X sge %08X pages %08X %08X %08X contig_hw %08X\n", v, ba, sge,
                        ldl_le_phys(address_space_memory, sge + (ba / 4096) * 8),
                        ldl_le_phys(address_space_memory, sge + (ba / 4096 + 1) * 8),
                        ldl_le_phys(address_space_memory, sge + (ba / 4096 + 2) * 8),
                        xbox_ContiguousAllocatedBytes());
                if (((fmt >> 30) & 3) == 2) {
                    /* ADPCM: how many of the buffer's blocks carry a valid
                     * header (step index <= 88, reserved byte 0)? Garbage
                     * in the buffer decodes as noise. */
                    uint32_t ebo = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO);
                    uint32_t blocks = (ebo + 1) / 64, ok = 0, b;
                    for (b = 0; b < blocks; b++) {
                        uint32_t w = ldl_le_phys(address_space_memory, get_data_ptr(sge, 0xFFFFFFFF, ba + b * 36));
                        if (((w >> 16) & 0xFF) <= 88 && (w >> 24) == 0) ok++;
                    }
                    fprintf(stderr, "[VOICE] %03X adpcm blocks valid %u / %u\n", v, ok, blocks);
                    if (ok < blocks) {
                        /* The first invalid blocks, and where the bad run starts and ends. */
                        uint32_t shown = 0, run_lo = 0xFFFFFFFFu, run_hi = 0;
                        for (b = 0; b < blocks; b++) {
                            hwaddr pa = get_data_ptr(sge, 0xFFFFFFFF, ba + b * 36);
                            uint32_t w = ldl_le_phys(address_space_memory, pa);
                            if (((w >> 16) & 0xFF) <= 88 && (w >> 24) == 0) continue;
                            if (b < run_lo) run_lo = b;
                            run_hi = b;
                            if (shown++ < 2) {
                                const uint8_t *q = apu_phys(pa);
                                fprintf(stderr, "[VOICE] %03X bad block %u pa %08X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                                        v, b, (unsigned)pa, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7],
                                        q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
                            }
                        }
                        fprintf(stderr, "[VOICE] %03X bad blocks span %u..%u, cbo %06X\n", v, run_lo, run_hi,
                                voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO));
                        {
                            /* Which byte shift makes the buffer valid ADPCM? */
                            uint32_t sh, best = 0, best_ok = 0;
                            for (sh = 0; sh < 36; sh++) {
                                uint32_t n = 0;
                                for (b = 0; b + 1 < blocks; b++) {
                                    uint32_t w2 = ldl_le_phys(address_space_memory, get_data_ptr(sge, 0xFFFFFFFF, ba + b * 36 + sh));
                                    if (((w2 >> 16) & 0xFF) <= 88 && (w2 >> 24) == 0) n++;
                                }
                                if (n > best_ok) { best_ok = n; best = sh; }
                            }
                            fprintf(stderr, "[VOICE] %03X best shift %u (%u valid)\n", v, best, best_ok);
                        }
                    }
                    {
                        /* The voice's scatter-gather pages should run on
                         * consecutively from the first; list any that don't. */
                        uint32_t first = ldl_le_phys(address_space_memory, sge + (ba / 4096) * 8), pg;
                        uint32_t npages = (ba % 4096 + blocks * 36 + 4095) / 4096;
                        for (pg = 0; pg < npages; pg++) {
                            uint32_t e = ldl_le_phys(address_space_memory, sge + (ba / 4096 + pg) * 8);
                            if (e != first + pg * 4096)
                                fprintf(stderr, "[VOICE] %03X sge page %u = %08X (expected %08X)\n", v, pg, e, first + pg * 4096);
                        }
                    }
                }
            }
            calls[v] = 0;
            frames_at[v] = g_apu_vp_frames;
        }
    }

    /* Step amplitude envelope */
    float ea_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    if (ea_value < 0.0f) ea_value = 0.0f;
    if (ea_value > 1.0f) ea_value = 1.0f;

    float samples[NUM_SAMPLES_PER_FRAME][2];
    memset(samples, 0, sizeof(samples));

    bool multipass = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS) != 0;
    dbg->multipass = multipass;

    if (multipass) {
        /* Read from multipass bin */
        int mp_bin = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        dbg->multipass_bin = (uint8_t)mp_bin;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            samples[i][0] = mixbins[mp_bin][i];
            samples[i][1] = mixbins[mp_bin][i];
        }
        bool clear_mix = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX) != 0;
        if (clear_mix) {
            memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
        }
    } else {
        int empty = 0;
        for (int sample_count = 0; sample_count < NUM_SAMPLES_PER_FRAME;) {
            int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            if (!active) return;
            int count = voice_resample(d, v, &samples[sample_count],
                                       NUM_SAMPLES_PER_FRAME - sample_count, rate);
            if (count < 0) break;
            /* An active voice that keeps producing nothing is an underrun, not
             * a reason to spin holding d->lock (xboxrecomp main 1041f71). */
            if (count == 0 && ++empty > 4) break;
            sample_count += count;
        }
    }

    /* A voice that ended during this frame still mixes the frame: its last
     * samples are in it (the resampler pads past the end with silence).
     * Returning here when the voice went inactive used to drop the final
     * up-to-31 samples of every one-shot -- every effect, call and chant
     * ended with a hard cut (2026-10-15). The voice was active when this
     * frame started (checked by the fetch loop above). */

    /* Get volume bins */
    int bin[8];
    bin[0] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5);
    vol[6] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8);
    vol[6] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4);
    vol[6] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8);
    vol[7] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4);
    vol[7] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        dbg->bin[i] = (uint8_t)bin[i];
        dbg->vol[i] = vol[i];
    }
    if (pitch_log_on())
        for (int i = 0; i < 8; i++)
            if (bin[i] == 2 && attenuate(vol[i]) > 0.001f) s_pstat[v].c_bin = 1;
    {
        /* RECOMP_APU_VOICE_LOG: where each voice goes (bin:gain), every 5 s. */
        static DWORD last_route[MCPX_HW_MAX_VOICES];
        if (getenv("RECOMP_APU_VOICE_LOG") && GetTickCount() - last_route[v] > 5000) {
            char buf[256];
            int len = 0;
            last_route[v] = GetTickCount();
            for (int i = 0; i < 8; i++)
                if (attenuate(vol[i]) > 0.001f)
                    len += snprintf(buf + len, sizeof buf - len, " %d:%.2f", bin[i], attenuate(vol[i]));
            fprintf(stderr, "[ROUTE] t=%lu %03X ebo %06X csize %u rate %.3f ->%s\n", GetTickCount(), v,
                    voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO),
                    voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE),
                    rate, len ? buf : " (silent)");
        }
    }

    if (voice_should_mute(v)) return;

    /* A broadcast theme replaces the game's own music (menus and pregame):
     * NFL 2K5 streams it on voices 0x44/0x45 (left/right), commentary and
     * crowd on others (measured 2026-09-26, RECOMP_APU_VOICE_LOG). */
    if (g_apu_mute_music && (v == 0x44 || v == 0x45)) return;
    /* Its pregame cues (team cards etc.) come on pooled voices (0x50/0x51
     * measured) that are later reused for effects, so they are told apart by
     * routing: game music is a stereo pair, each half sent to one front
     * speaker only (bin 0 or bin 1); commentary is centred, crowd and effects
     * go to both sides. Exception: the stadium crowd beds are hard-panned
     * pairs too, on 0x4A..0x4F from match load to the end (measured). */
    if (g_apu_mute_music && v >= MCPX_HW_MAX_3D_VOICES && !(v >= 0x4A && v <= 0x4F)) {
        int left = 0, right = 0, other = 0;
        for (int i = 0; i < 8; i++) {
            if (attenuate(vol[i]) <= 0.001f) continue;
            if (bin[i] == 0) left = 1;
            else if (bin[i] == 1) right = 1;
            else other = 1;
        }
        if (!other && (left != right)) {
            static DWORD last_log[MCPX_HW_MAX_VOICES];
            if (getenv("RECOMP_APU_VOICE_LOG") && GetTickCount() - last_log[v] > 5000) {
                last_log[v] = GetTickCount();
                fprintf(stderr, "[ROUTE] t=%lu %03X muted (game music under a broadcast theme)\n", GetTickCount(), v);
            }
            return;
        }
    }

    if (diag_on()) {   /* audio report: loudest bin volume (smallest attenuation) and envelope */
        DiagVoice *dv = &s_dvoice[v];
        for (int i = 0; i < 8; i++)
            if ((vol[i] & 0xFFF) < dv->vmin) dv->vmin = (uint16_t)(vol[i] & 0xFFF);
        if (ea_value > dv->emax) dv->emax = ea_value;
    }

    /* RECOMP_APU_PITCH_LOG: the gain this voice is heard at (after the mutes above). */
    if (pitch_log_on()) {
        float g = 0.0f;
        for (int i = 0; i < 8; i++)
            if (attenuate(vol[i]) > g) g = attenuate(vol[i]);
        g *= ea_value;
        if (g > s_pstat[v].gmax) s_pstat[v].gmax = g;
        s_pstat[v].gsum += g;
    }

    /* Low-pass filter */
    int fmode = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);
    bool lpf = false;
    if (v < MCPX_HW_MAX_3D_VOICES) {
        lpf = (fmode == 1);
    } else {
        lpf = stereo ? (fmode == 1) : (fmode & 1) != 0;
    }
    if (lpf) {
        for (int ch = 0; ch < 2; ch++) {
            int16_t fc = (int16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC0);
            float fc_f = clampf(powf(2, fc / 4096.0f), 0.003906f, 1.0f);
            uint16_t q = (uint16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC1);
            float q_f = clampf(q / (1.0f * 0x8000), 0.079407f, 1.0f);
            sv_filter *filter = &d->vp.filters[v].svf[ch];
            setup_svf(filter, fc_f, q_f, F_LP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                samples[i][ch] = run_svf(filter, samples[i][ch]);
                samples[i][ch] = fminf(fmaxf(samples[i][ch], -1.0f), 1.0f);
            }
        }
    }

    /* HRTF processing for 3D voices */
    if (v < MCPX_HW_MAX_3D_VOICES && g_config.audio.hrtf) {
        uint16_t hrtf_handle =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                     NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        if (hrtf_handle != HRTF_NULL_HANDLE) {
            hrtf_filter_process(&d->vp.filters[v].hrtf, samples, samples);
        }
    }

    /* RECOMP_APU_VOICE_DUMP=<dir>: each voice's output (left, at its largest
     * bin gain) to <dir>\v<handle>.raw as records of {uint32 frame, int16[32]},
     * to find which voice carries a sound in a recording. */
    {
        static int on = -1;
        static char dir[260];
        static FILE *f[MCPX_HW_MAX_VOICES];
        extern unsigned long g_apu_vp_frames;
        if (on < 0) {
            const char *e = getenv("RECOMP_APU_VOICE_DUMP");
            on = e && *e;
            if (on) snprintf(dir, sizeof dir, "%s", e);
        }
        if (on && v < MCPX_HW_MAX_VOICES) {
            if (!f[v]) {
                char p[300];
                snprintf(p, sizeof p, "%s\\v%03X.raw", dir, v);
                f[v] = fopen(p, "wb");
            }
            if (f[v]) {
                float g = 0.0f;
                for (int b = 0; b < 8; b++) {
                    float a = attenuate(vol[b]);
                    if (a > g) g = a;
                }
                g *= ea_value;
                uint32_t fr = (uint32_t)g_apu_vp_frames;
                int16_t out[NUM_SAMPLES_PER_FRAME];
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                    out[i] = (int16_t)(clampf(g * samples[i][0], -1.0f, 1.0f) * 32767.0f);
                fwrite(&fr, 4, 1, f[v]);
                fwrite(out, 2, NUM_SAMPLES_PER_FRAME, f[v]);
            }
        }
    }

    /* Mix into bins */
    for (int b = 0; b < 8; b++) {
        float g = ea_value;
        float hr;
        if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4)) {
            hr = (float)(1 << d->vp.hrtf_headroom);
        } else {
            hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
        }
        g *= attenuate(vol[b]) / hr;
        /* Volume ramp: the gain moves from last frame's value to this one
         * across the 32 samples instead of jumping at the frame boundary.
         * The jumps clicked on every volume update (crowd swells and fades,
         * moving 3D effects, DirectSound volume steps), and a voice started
         * at full gain mid-waveform clicked too (now it fades in over one
         * frame, 0.7 ms). A changed bin routing starts from the new gain. */
        {
            float g0 = s_bin_gain[v][b];
            if (s_bin_gain_bin[v][b] != (uint8_t)bin[b]) {
                g0 = (g0 == 0.0f) ? 0.0f : g;
                s_bin_gain_bin[v][b] = (uint8_t)bin[b];
            }
            float dg = (g - g0) / NUM_SAMPLES_PER_FRAME;
            if (g0 == 0.0f && g == 0.0f) continue;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                g0 += dg;
                mixbins[bin[b]][i] += g0 * samples[i][b % channels];
            }
            s_bin_gain[v][b] = g;
        }
    }

    /* VP monitor mix */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        float g = 0.0f;
        for (int b = 0; b < 8; b++) {
            float hr = (float)(1 << d->vp.submix_headroom[bin[b]]);
            float bg = attenuate(vol[b]) / hr;
            if (bg > g) g = bg;
        }
        g *= ea_value;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            sample_buf[i][0] += g * samples[i][0];
            sample_buf[i][1] += g * samples[i][1];
        }
    }

    (void)voice_list;
}

/* ============================================================
 * VP Frame - Process all voice lists
 *
 * Simplified single-threaded version (no worker threads initially)
 * ============================================================ */

unsigned long g_apu_vp_frames;   /* VP frames run (RECOMP_APU_VOICE_LOG) */
int64_t g_apu_lock_wait_us;       /* time the VP waited on DirectSound voice locks (RECOMP_APU_PITCH_LOG) */
unsigned g_apu_lock_wait_n;

void mcpx_apu_vp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
    static unsigned long frames, processed, idle, last_ms;
    frames++;
    g_apu_vp_frames++;
    {
        unsigned long now = GetTickCount();
        extern int xbox_verbose(void);
        if (now - last_ms >= 5000 && xbox_verbose()) {
            extern uint32_t xbox_ContiguousAllocatedBytes(void);
            fprintf(stderr, "[APU] vp frames=%lu voices processed=%lu idle=%lu ISTS=%08X IEN=%08X"
                    " FECTL=%08X FENADDR=%08X tops=%04X/%04X/%04X"
                    " VPVADDR=%08X contig_hw=%08X\n", frames, processed, idle,
                    (unsigned)d->regs[NV_PAPU_ISTS], (unsigned)d->regs[NV_PAPU_IEN],
                    (unsigned)d->regs[NV_PAPU_FECTL], (unsigned)d->regs[NV_PAPU_FENADDR],
                    (unsigned)d->regs[voice_list_regs[0].top], (unsigned)d->regs[voice_list_regs[1].top],
                    (unsigned)d->regs[voice_list_regs[2].top], (unsigned)d->regs[NV_PAPU_VPVADDR],
                    xbox_ContiguousAllocatedBytes());
            /* The active voices and where each is playing (CBO), EBO end,
             * paused bit -- speech that never advances shows up here. */
            for (int l = 0; l < 3; l++) {
                uint16_t v = (uint16_t)d->regs[voice_list_regs[l].top];
                for (int k = 0; v != 0xFFFF && k < 24; k++) {
                    fprintf(stderr, "[APU]   list%d voice %03X cbo=%06X ebo=%06X state=%08X\n", l, v,
                            voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO),
                            voice_get_mask(d, v, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO),
                            voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, 0xFFFFFFFFu));
                    v = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                                 NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
                }
            }
            last_ms = now;
        }
    }

    for (int list = 0; list < 3; list++) {
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint16_t v = (uint16_t)d->regs[current];
            d->regs[next] = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);

            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                idle++;
                fe_method(d, SE2FE_IDLE_VOICE, v);
            } else {
                /* DirectSound locks a voice (PIO VOICE_LOCK) while it reads
                 * or rewrites the voice record directly (play cursor,
                 * position, buffer). As on the console (and xemu), the VP
                 * leaves a locked voice alone: wait for the unlock briefly,
                 * else skip it this frame. Processing it anyway wrote stale
                 * positions over the title's, and NFL 2K5's music streamer
                 * restarted songs every 1-5 s (2026-10-01). */
                static unsigned long lock_waits, lock_skips;
                static DWORD lock_log;
                bool skip = false;
                if (is_voice_locked(d, v)) {
                    int64_t t0 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
                    int64_t until = t0 + 3000;
                    lock_waits++;
                    while (is_voice_locked(d, v) && qemu_clock_get_us(QEMU_CLOCK_REALTIME) < until)
                        qemu_cond_timedwait(&d->cond, &d->lock, 1);
                    skip = is_voice_locked(d, v);
                    if (skip) lock_skips++;
                    g_apu_lock_wait_us += qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0;
                    g_apu_lock_wait_n++;
                    g_apu_dg_lockwait_us += qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0;
                    g_apu_dg_lockwait_n++;
                }
                if (getenv("RECOMP_APU_VOICE_LOG") && GetTickCount() - lock_log > 5000) {
                    lock_log = GetTickCount();
                    fprintf(stderr, "[APU] voice locks: waited %lu, skipped %lu\n", lock_waits, lock_skips);
                }
                if (!skip) {
                    /* Process voice directly (single-threaded) */
                    processed++;
                    voice_process(d, mixbins, d->vp.sample_buf, v, list);
                }
            }
            d->regs[current] = d->regs[next];
        }
    }

    pitch_stat_report(d);
    diag_report();

    /* VP monitor output */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] += isamp[2 * i];
            d->monitor.frame_buf[off + i][1] += isamp[2 * i + 1];
        }
        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float) * NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    }
}

/* ============================================================
 * VP Init / Finalize / Reset
 * ============================================================ */

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    /* Single-threaded - no worker dispatch needed */
    (void)d;
}

void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    (void)d;
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset(d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        hrtf_filter_init(&d->vp.filters[v].hrtf);
    }
}
