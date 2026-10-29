/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* Initialize XAudio2. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);

/* Sample frames queued ahead of the speakers, or -1 (inactive / not consuming). */
int xa2_queued_samples(void);
/* The queue fill the APU paces itself to (RECOMP_AUDIO_TARGET_MS), in frames. */
int xa2_target_samples(void);
/* RECOMP_AUDIO_LEGACY=1: the pre-2026-10-22 output (1024-sample buffers, wall-clock pacing). */
int xa2_legacy(void);

/* Audio report (AUDIO-REPORT-HOTKEY-20261022): one diagnostics record per second. */
#include <stddef.h>
#define XA2_DIAG_VOICES 16
typedef struct {
    uint32_t tick;              /* GetTickCount at the end of the second */
    uint64_t frames_out;        /* report-ring position (output frames) then */
    float fps, worst_ms;        /* game frames presented that second, longest frame */
    uint32_t vpframes;          /* APU voice-processor frames (1500 nominal) */
    int qmin, qmax;             /* speaker queue at submissions, frames */
    uint32_t underruns, drops;
    uint32_t silent_vp;         /* VP frames emitted silent after a 40 ms ISR wait */
    uint32_t stalls;            /* waits for the title's ISR, count and ms */
    float stall_ms;
    uint32_t lockwaits;         /* VP waits on DirectSound voice locks */
    float lockwait_ms;
    uint16_t active, nchg;      /* voices playing; voices whose rate changed */
    struct { uint16_t v; float rmin, rmax, gain; } chg[XA2_DIAG_VOICES];  /* rate in Hz */
} XA2DiagSec;
uint64_t xa2_report_frames(void);
void xa2_diag_take(int *qmin, int *qmax, unsigned *under, unsigned *drops);
void xa2_diag_push(const XA2DiagSec *rec);
int xbox_AudioReportSave(char *name_out, size_t n);

#endif /* APU_XAUDIO2_H */
