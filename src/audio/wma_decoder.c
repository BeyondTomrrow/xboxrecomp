#include "wma_decoder.h"

#include <stdlib.h>
#include <string.h>

static int wma_append(XboxWmaPcm *out, size_t *capacity,
                      const uint8_t *data, size_t size)
{
    size_t needed;
    size_t next;
    uint8_t *grown;

    if (size == 0) return 0;
    if (out->size > (size_t)-1 - size) return -1;
    needed = out->size + size;

    if (needed > *capacity) {
        next = *capacity ? *capacity : 64 * 1024;
        while (next < needed) {
            if (next > (size_t)-1 / 2) {
                next = needed;
                break;
            }
            next *= 2;
        }
        grown = (uint8_t *)realloc(out->data, next);
        if (!grown) return -1;
        out->data = grown;
        *capacity = next;
    }

    memcpy(out->data + out->size, data, size);
    out->size += size;
    return 0;
}

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

static wchar_t *wma_to_wide(const char *s)
{
    int len;
    wchar_t *w;

    if (!s) return NULL;
    len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (len <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)len * sizeof(wchar_t));
    if (!w) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len)) {
        free(w);
        return NULL;
    }
    return w;
}

static int wma_read_format(IMFSourceReader *reader, XboxWmaPcm *out)
{
    IMFMediaType *type = NULL;
    UINT32 rate = 0;
    UINT32 channels = 0;
    UINT32 bits = 0;
    HRESULT hr;

    hr = IMFSourceReader_GetCurrentMediaType(
        reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &type);
    if (FAILED(hr) || !type) return -1;

    hr = IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);

    IMFMediaType_Release(type);

    if (FAILED(hr) || rate == 0 || channels == 0 || bits == 0 ||
        channels > 0xFFFFu || bits > 0xFFFFu)
        return -1;

    out->sample_rate = rate;
    out->channels = (uint16_t)channels;
    out->bits_per_sample = (uint16_t)bits;
    return 0;
}

static int wma_same_format(const XboxWmaPcm *a, const XboxWmaPcm *b)
{
    return a->sample_rate == b->sample_rate &&
           a->channels == b->channels &&
           a->bits_per_sample == b->bits_per_sample;
}

int xbox_wma_decode_file(const char *path, XboxWmaPcm *out)
{
    HRESULT hr;
    HRESULT co_hr;
    int co_owned = 0;
    int mf_started = 0;
    wchar_t *wpath = NULL;
    IMFSourceReader *reader = NULL;
    IMFMediaType *pcm_type = NULL;
    size_t capacity = 0;
    int result = -1;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!path) return -1;

    co_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(co_hr)) {
        co_owned = 1;
    } else if (co_hr != RPC_E_CHANGED_MODE) {
        goto done;
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) goto done;
    mf_started = 1;

    wpath = wma_to_wide(path);
    if (!wpath) goto done;

    hr = MFCreateSourceReaderFromURL(wpath, NULL, &reader);
    if (FAILED(hr) || !reader) goto done;

    IMFSourceReader_SetStreamSelection(
        reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    hr = IMFSourceReader_SetStreamSelection(
        reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (FAILED(hr)) goto done;

    hr = MFCreateMediaType(&pcm_type);
    if (FAILED(hr) || !pcm_type) goto done;

    hr = IMFMediaType_SetGUID(pcm_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetGUID(pcm_type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    if (FAILED(hr)) goto done;

    hr = IMFSourceReader_SetCurrentMediaType(
        reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pcm_type);
    if (FAILED(hr)) goto done;

    if (wma_read_format(reader, out) != 0) goto done;

    for (;;) {
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample *sample = NULL;

        hr = IMFSourceReader_ReadSample(
            reader,
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0,
            &stream_index,
            &flags,
            &timestamp,
            &sample);
        (void)stream_index;
        (void)timestamp;
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
            if (sample) IMFSample_Release(sample);
            goto done;
        }

        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            XboxWmaPcm changed;
            memset(&changed, 0, sizeof(changed));
            if (wma_read_format(reader, &changed) != 0) {
                if (sample) IMFSample_Release(sample);
                goto done;
            }
            if (out->size != 0 && !wma_same_format(out, &changed)) {
                if (sample) IMFSample_Release(sample);
                goto done;
            }
            out->sample_rate = changed.sample_rate;
            out->channels = changed.channels;
            out->bits_per_sample = changed.bits_per_sample;
        }

        if (sample) {
            IMFMediaBuffer *buffer = NULL;
            BYTE *bytes = NULL;
            DWORD max_len = 0;
            DWORD cur_len = 0;

            hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer);
            if (FAILED(hr) || !buffer) {
                IMFSample_Release(sample);
                goto done;
            }
            hr = IMFMediaBuffer_Lock(buffer, &bytes, &max_len, &cur_len);
            (void)max_len;
            if (FAILED(hr)) {
                IMFMediaBuffer_Release(buffer);
                IMFSample_Release(sample);
                goto done;
            }
            if (wma_append(out, &capacity, bytes, (size_t)cur_len) != 0) {
                IMFMediaBuffer_Unlock(buffer);
                IMFMediaBuffer_Release(buffer);
                IMFSample_Release(sample);
                goto done;
            }
            IMFMediaBuffer_Unlock(buffer);
            IMFMediaBuffer_Release(buffer);
            IMFSample_Release(sample);
        }

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }

    if (out->size == 0) goto done;
    result = 0;

done:
    if (pcm_type) IMFMediaType_Release(pcm_type);
    if (reader) IMFSourceReader_Release(reader);
    free(wpath);
    if (mf_started) MFShutdown();
    if (co_owned) CoUninitialize();
    if (result != 0) xbox_wma_pcm_free(out);
    return result;
}

#else /* POSIX: Decode WMA/ASF to 16-bit 44.1 kHz Stereo PCM via FFmpeg pipe */

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int xbox_wma_decode_file(const char *path, XboxWmaPcm *out)
{
    int pipefd[2];
    posix_spawn_file_actions_t actions;
    pid_t pid = 0;
    size_t capacity = 0;
    uint8_t chunk[16384];
    ssize_t n;
    int status = 0;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!path) return -1;

    if (pipe(pipefd) != 0)
        return -1;

    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    const char *argv[] = {
        "ffmpeg",
        "-nostdin",
        "-v", "error",
        "-i", path,
        "-vn",
        "-f", "s16le",
        "-acodec", "pcm_s16le",
        "-ar", "44100",
        "-ac", "2",
        "pipe:1",
        NULL
    };

    if (posix_spawnp(&pid, "ffmpeg", &actions, NULL, (char *const *)argv, environ) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]); /* Close write end in parent */

    while ((n = read(pipefd[0], chunk, sizeof(chunk))) > 0) {
        if (wma_append(out, &capacity, chunk, (size_t)n) != 0) {
            close(pipefd[0]);
            waitpid(pid, &status, 0);
            xbox_wma_pcm_free(out);
            return -1;
        }
    }
    close(pipefd[0]);

    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || out->size == 0) {
        xbox_wma_pcm_free(out);
        return -1;
    }

    out->sample_rate = 44100;
    out->channels = 2;
    out->bits_per_sample = 16;
    return 0;
}

#endif

void xbox_wma_pcm_free(XboxWmaPcm *pcm)
{
    if (!pcm) return;
    free(pcm->data);
    memset(pcm, 0, sizeof(*pcm));
}