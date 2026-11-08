/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/xbox_winnt.h"
#endif
#ifdef _WIN32
#include "apu_env.h"     /* cached getenv: debug switches are read per voice */
#endif

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 *
 * The address is not derivable from the APU registers: GPSADDR/GPFADDR/
 * EPSADDR/EPFADDR point at the DSP's own scratch and frame memory, while the
 * command block is a DirectSound heap allocation. On Wreckless the registers
 * read 0x01504000 / 0x014EC000 / 0x0151C000 / 0x014F0000 and the doorbell is at
 * 0x014F8810 -- inside none of them. So it has to be observed: run with
 * RECOMP_WATCHDOG_SECS and the spin shows up as ebx plus the poll offset.
 */
#define APU_DSP_ACK_MAX 8
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

/* Broadcast mix controls (xbox_AudioSetGameGain / xbox_AudioGameLevel in
 * apu_core.c): bed gain for everything but the commentary, and the smoothed
 * level of the commentary (centre bin). */
/* Stand-in for the GP's I3DL2 reverb (the GP is not emulated): a small
 * Schroeder reverb (4 combs + 2 all-passes per side, different lengths left
 * and right) fed by mixbin 10, the I3DL2 send. Only the wet signal is
 * output; the dry sound already reaches the speakers through its own bins. */
#define RV_COMBS 4
#define RV_APS 2
static const int rv_comb_len[2][RV_COMBS] = { { 1557, 1617, 1491, 1422 }, { 1580, 1640, 1514, 1445 } };
static const int rv_ap_len[2][RV_APS] = { { 556, 441 }, { 579, 464 } };
static float rv_comb_buf[2][RV_COMBS][1700], rv_comb_lp[2][RV_COMBS];
static int rv_comb_pos[2][RV_COMBS];
static float rv_ap_buf[2][RV_APS][600];
static int rv_ap_pos[2][RV_APS];

static float reverb_tick(int side, float in)
{
    float out = 0.0f;
    int k;
    for (k = 0; k < RV_COMBS; k++) {
        float *b = rv_comb_buf[side][k];
        int p = rv_comb_pos[side][k];
        float y = b[p];
        rv_comb_lp[side][k] = y * 0.6f + rv_comb_lp[side][k] * 0.4f;    /* damping */
        b[p] = in + rv_comb_lp[side][k] * 0.78f;                         /* feedback */
        if (++p >= rv_comb_len[side][k]) p = 0;
        rv_comb_pos[side][k] = p;
        out += y;
    }
    out *= 0.25f;
    for (k = 0; k < RV_APS; k++) {
        float *b = rv_ap_buf[side][k];
        int p = rv_ap_pos[side][k];
        float y = b[p];
        b[p] = out + y * 0.5f;
        out = y - out * 0.5f;
        if (++p >= rv_ap_len[side][k]) p = 0;
        rv_ap_pos[side][k] = p;
    }
    return out;
}

volatile float g_apu_bed_gain = 1.0f;
volatile float g_apu_voice_gain = 1.0f;   /* commentary (centre bin) */
volatile float g_apu_voice_level;

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    {
        double e = 0;
        float r, a;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            e += (double)mixbins[2][i] * mixbins[2][i];
        r = (float)sqrt(e / NUM_SAMPLES_PER_FRAME);
        a = r > g_apu_voice_level ? 0.3f : 0.004f;    /* fast attack, slow release */
        g_apu_voice_level += (r - g_apu_voice_level) * a;
    }

    {
        /* RECOMP_APU_BIN_LOG=1: RMS of each mixbin every 2 s, to see which
         * bins carry what (commentary, crowd, music, effect sends). */
        static int on = -1;
        static double e[NUM_MIXBINS];
        static unsigned long n;
        static DWORD last;
        if (on < 0) on = getenv("RECOMP_APU_BIN_LOG") != NULL;
        if (on) {
            for (int b = 0; b < NUM_MIXBINS; b++)
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                    e[b] += (double)mixbins[b][i] * mixbins[b][i];
            n += NUM_SAMPLES_PER_FRAME;
            if (GetTickCount() - last > 2000) {
                char line[512];
                int len = 0;
                for (int b = 0; b < NUM_MIXBINS; b++) {
                    double r = sqrt(e[b] / (double)(n ? n : 1));
                    if (r > 0.0005)
                        len += snprintf(line + len, sizeof line - len, " %d:%.3f", b, r);
                    e[b] = 0;
                }
                n = 0;
                last = GetTickCount();
                fprintf(stderr, "[BINS]%s\n", len ? line : " silent");
            }
        }
    }

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * Even bins left, odd bins right, which preserves the stereo
             * pairing the guest set up -- bins 6/7 and 8/9 arrive with matched
             * counts. This is not what a real EP does; it is the cheapest
             * mixdown that stops discarding audio. */
            float left, right;
            if (mcpx_apu_mixdown_all()) {
                /* Bins 0-5 are the speaker feeds (FL, FR, C, LFE, SL, SR).
                 * NFL 2K5 puts its commentary on the centre bin (2), crowd,
                 * music and stings on 0/1: the centre goes to both sides at
                 * -3 dB (it used to land in the left ear only), LFE to both
                 * at half, and everything but the centre is scaled by the
                 * "bed" gain so a broadcast theme can replace the game's
                 * music under the announcers (xbox_AudioSetGameGain). */
                /* DirectSound's mixbins: 0 FL, 1 FR, 2 C, 3 LFE, 4 BL, 5 BR,
                 * 6-9 the 3D (crosstalk) FL/FR/BL/BR feeds, 10 the I3DL2
                 * reverb send, 11-31 effect sends. The sends are copies of
                 * voices meant for the GP's effects, not speaker feeds: they
                 * used to be added dry, even bins to the left ear and odd to
                 * the right, so the reverb send doubled every 3D effect in
                 * the left ear and effect send 0 sat in the right ear alone
                 * (measured in a match, 2026-10-15: bins 6/7 0.17, bin 10
                 * 0.05, bin 11 0.01 RMS). Now: speaker and 3D bins to their
                 * sides, the reverb send through a reverb (wet only), effect
                 * sends centred at -6 dB. */
                float bed = g_apu_bed_gain, c = mixbins[2][i] * g_apu_voice_gain, lfe = mixbins[3][i] * 0.5f;
                float fx = 0.0f, rv = mixbins[10][i];
                for (int b = 11; b < NUM_MIXBINS; ++b) fx += mixbins[b][i];
                fx *= 0.5f;
                left = mixbins[0][i] + mixbins[4][i] + mixbins[6][i] + mixbins[8][i];
                right = mixbins[1][i] + mixbins[5][i] + mixbins[7][i] + mixbins[9][i];
                left += reverb_tick(0, rv) * 0.7f + fx;
                right += reverb_tick(1, rv) * 0.7f + fx;
                left = (left + lfe) * bed + 0.7071f * c;
                right = (right + lfe) * bed + 0.7071f * c;
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            /* Summing every bin overdrives: soft-limit instead of hard
             * clipping (x / (1 + |x|) above 0.5), which kept peaks at full
             * scale in a match (2026-09-26). */
            {
                float *ch[2] = { &left, &right };
                for (int c = 0; c < 2; c++) {
                    float x = *ch[c], ax = x < 0 ? -x : x;
                    if (ax > 0.5f) {
                        float over = ax - 0.5f;
                        ax = 0.5f + over / (1.0f + 2.0f * over);   /* approaches 1.0 */
                        *ch[c] = x < 0 ? -ax : ax;
                    }
                }
            }
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Output low-pass at ~11 kHz. On the console nothing reaches the
             * speakers above ~10-11 kHz: the voice sources are <=22 kHz
             * (11 kHz Nyquist) and the VP/GP band-limit at the source rate, so
             * xemu's output falls off a cliff by 11 kHz (11-15 kHz at -100 dBFS
             * in a match). Ours sat 25-40 dB higher -- cubic-interpolation
             * images (now band-limited per voice) and harmonics the soft limiter
             * adds above the knee. A 4th-order Butterworth here matches the
             * cliff for every voice at once, after the limiter that creates the
             * harmonics. RECOMP_APU_NO_OUTLP=1 turns it off.
             * docs/AUDIO-USER-REPORTS-20261022.md. */
            {
                static int off_lp = -1;
                static float b0[2], b1[2], b2[2], a1[2], a2[2];
                static float z[2][2][2];   /* [section][channel][2] */
                if (off_lp < 0) {
                    off_lp = getenv("RECOMP_APU_NO_OUTLP") != NULL;
                    float fc = 11000.0f, w0 = 2.0f * 3.14159265f * fc / 48000.0f;
                    float cw = cosf(w0), sw = sinf(w0);
                    static const float q[2] = { 0.54119610f, 1.30656296f };   /* 4th-order Butterworth */
                    for (int sct = 0; sct < 2; sct++) {
                        float alpha = sw / (2.0f * q[sct]), a0 = 1.0f + alpha;
                        b0[sct] = (1.0f - cw) * 0.5f / a0; b1[sct] = (1.0f - cw) / a0; b2[sct] = b0[sct];
                        a1[sct] = -2.0f * cw / a0; a2[sct] = (1.0f - alpha) / a0;
                    }
                }
                if (!off_lp) {
                    float *ch[2] = { &left, &right };
                    for (int c = 0; c < 2; c++) {
                        float x = *ch[c];
                        for (int sct = 0; sct < 2; sct++) {
                            float y = b0[sct] * x + z[sct][c][0];
                            z[sct][c][0] = b1[sct] * x - a1[sct] * y + z[sct][c][1];
                            z[sct][c][1] = b2[sct] * x - a2[sct] * y;
                            x = y;
                        }
                        *ch[c] = x;
                    }
                }
            }
            /* Master trim to xemu's level. A moderate demo play measured ~3-4 dB
             * hotter than xemu in a matched 20 s window (mono -23.6 vs -28.0 dBFS,
             * 2-8 kHz -32.3 vs -36.8); -2.5 dB brings the loud scenes in line while
             * leaving the near-equal quiet scenes only slightly under. Env override
             * RECOMP_APU_OUT_GAIN=<linear> (1.0 = none). docs/AUDIO-USER-REPORTS-20261022.md. */
            {
                static float g = -1.0f;
                if (g < 0.0f) { const char *e = getenv("RECOMP_APU_OUT_GAIN"); g = e ? (float)atof(e) : 0.75f; }
                left *= g; right *= g;
            }

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
