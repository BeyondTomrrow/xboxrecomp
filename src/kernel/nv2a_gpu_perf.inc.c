/* ── Frame statistics (2026-10-09) ─────────────────────────────────────────
 *
 * Included into nv2a_gpu_present.inc.c. Measures what a PC benchmark reports:
 *
 *   - frame time: the interval between presents, in a ring of the last
 *     PERF_RING frames -> average FPS, 1% and 0.1% lows (the FPS of the
 *     99th / 99.9th percentile frame time), worst frame;
 *   - GPU time per frame: the 3D engine's busy share (Windows' GPU Engine
 *     counter for this process) times the frame time;
 *   - CPU: process CPU time per presented frame (all threads), and the time
 *     the render thread spends between presents executing the pushbuffer;
 *   - memory: working set, private bytes, and the adapter's local video
 *     memory in use (DXGI 1.4 budget query).
 *
 * Every 10 s the window's numbers go to the log as one [BENCH] line, which
 * tools/bench.ps1 parses. NFL2K5_FRAMETIME_CSV=<file> also writes every
 * frame (present time, frame ms, GPU ms) for graphs. The Show FPS overlay
 * reads the same numbers (perf_overlay_text).
 */

#include <dxgi1_4.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")

/* GPU busy time: Windows' per-process "GPU Engine" 3D utilisation counter
 * (what Task Manager shows). D3D11 timestamps around a frame turned out to
 * span command-buffer flushes, not GPU work, so they are not used for it. */
static struct {
    PDH_HQUERY q;
    PDH_HCOUNTER c;
    int state;                          /* 0 untried, 1 ok, -1 unavailable */
    float util;                         /* percent, last second */
} s_gpuctr;

static void perf_gpu_counter(void)
{
    if (s_gpuctr.state == 0) {
        wchar_t path[128];
        s_gpuctr.state = -1;
        swprintf(path, 128, L"\\GPU Engine(pid_%lu_*engtype_3D)\\Utilization Percentage", GetCurrentProcessId());
        if (PdhOpenQueryW(NULL, 0, &s_gpuctr.q) == ERROR_SUCCESS &&
            PdhAddEnglishCounterW(s_gpuctr.q, path, 0, &s_gpuctr.c) == ERROR_SUCCESS &&
            PdhCollectQueryData(s_gpuctr.q) == ERROR_SUCCESS)
            s_gpuctr.state = 1;
        return;
    }
    if (s_gpuctr.state != 1) return;
    if (PdhCollectQueryData(s_gpuctr.q) == ERROR_SUCCESS) {
        DWORD bytes = 0, count = 0;
        static BYTE buf[16384];
        PDH_FMT_COUNTERVALUE_ITEM_W *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)buf;
        bytes = sizeof buf;
        if (PdhGetFormattedCounterArrayW(s_gpuctr.c, PDH_FMT_DOUBLE, &bytes, &count, items) == ERROR_SUCCESS) {
            double sum = 0;
            DWORD i;
            for (i = 0; i < count; i++)
                if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                    sum += items[i].FmtValue.doubleValue;
            s_gpuctr.util = (float)(sum > 100.0 ? 100.0 : sum);
        }
    }
}

#define PERF_RING 4096                /* frames kept for the percentiles (~68 s at 60) */

static struct {
    LARGE_INTEGER freq, last;
    float ft[PERF_RING];              /* frame times, ms */
    unsigned n, head;
    /* 10-second window */
    LARGE_INTEGER win_t0;
    unsigned win_frames;
    double win_ft_sum; float win_ft_max;
    double win_gpu_sum; unsigned win_gpu_n;
    ULONGLONG win_cpu0;               /* process CPU, 100 ns units */
    /* published for the overlay (refreshed each second) */
    float avg_fps, low1, low01, ft_avg, ft_max, gpu_ms, cpu_ms;
    unsigned ws_mb, priv_mb, vram_mb;
    LARGE_INTEGER pub_t0;
    unsigned pub_frames;
    double pub_ft_sum;
    ULONGLONG pub_cpu0;
    FILE *csv;
    int csv_tried;
    IDXGIAdapter3 *adapter3;
    int adapter_tried;
} s_perf;

static ULONGLONG perf_process_cpu(void)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

static int perf_cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* FPS at the given percentiles of the last `last` frame times (0 = the whole
 * ring): 0.99 -> 1% low, 0.999 -> 0.1% low. */
static void perf_percentiles(unsigned last, float *low1, float *low01, float *worst)
{
    static float tmp[PERF_RING];
    unsigned n = s_perf.n < PERF_RING ? s_perf.n : PERF_RING, i;
    if (last && last < n) n = last;
    if (n < 10) { *low1 = *low01 = *worst = 0; return; }
    for (i = 0; i < n; i++) tmp[i] = s_perf.ft[(s_perf.head + PERF_RING - n + i) % PERF_RING];
    qsort(tmp, n, sizeof tmp[0], perf_cmp_float);
    {
        float p99 = tmp[(unsigned)((n - 1) * 0.99)], p999 = tmp[(unsigned)((n - 1) * 0.999)];
        *low1 = p99 > 0 ? 1000.0f / p99 : 0;
        *low01 = p999 > 0 ? 1000.0f / p999 : 0;
        *worst = tmp[n - 1];
    }
}

static void perf_memory(void)
{
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof pm);
    pm.cb = sizeof pm;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm)) {
        s_perf.ws_mb = (unsigned)(pm.WorkingSetSize >> 20);
        s_perf.priv_mb = (unsigned)(pm.PrivateUsage >> 20);
    }
    if (!s_perf.adapter_tried) {
        IDXGIDevice *dd = NULL;
        IDXGIAdapter *ad = NULL;
        s_perf.adapter_tried = 1;
        if (SUCCEEDED(ID3D11Device_QueryInterface(s_gpu11.dev, &IID_IDXGIDevice, (void **)&dd))) {
            if (SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad))) {
                IDXGIAdapter_QueryInterface(ad, &IID_IDXGIAdapter3, (void **)&s_perf.adapter3);
                IDXGIAdapter_Release(ad);
            }
            IDXGIDevice_Release(dd);
        }
    }
    if (s_perf.adapter3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO vi;
        if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(s_perf.adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi)))
            s_perf.vram_mb = (unsigned)(vi.CurrentUsage >> 20);
    }
}

/* A slow frame (> 25 ms once play is under way): what happened during it, so
 * hitches can be attributed -- guest file reads (kernel_file.c), texture
 * uploads, new render targets, pixel shader variants made ready, draws, and
 * the time Present itself blocked. */
extern volatile LONG64 g_xbox_read_qpc, g_xbox_read_bytes, g_xbox_read_count;
extern volatile LONG64 g_pb_exec_qpc, g_pb_qwait_qpc, g_pb_capture_qpc;   /* xbox_memory_layout.c */
extern volatile DWORD g_xbox_game_tid;                                       /* xbox_ggl.c */
LONGLONG xbox_ggl_waited_qpc(DWORD tid);
static void perf_hitch(float ms, float present_ms)
{
    static LONG64 rq, rb, rc, ex, qw, cp, gw;
    LONG64 lw = g_xbox_game_tid ? xbox_ggl_waited_qpc(g_xbox_game_tid) : 0;
    static uint32_t draws, texup, surfs;
    static LONG ps;
    static unsigned logged;
    LONG64 q = g_xbox_read_qpc, b = g_xbox_read_bytes, c = g_xbox_read_count;
    LONG64 e = g_pb_exec_qpc, w = g_pb_qwait_qpc, k = g_pb_capture_qpc;
    double f = 1000.0 / (double)s_perf.freq.QuadPart;
    if (ms > 25.0f && s_perf.n > 1800 && logged < 400) {
        logged++;
        fprintf(stderr, "  [HITCH] %.1f ms: present %.1f ms, reads %lld (%lld KB, %.1f ms), tex uploads %u, new targets %u, "
                        "ps ready %ld, draws %u; render thread %.1f ms, queue wait %.1f ms, capture %.1f ms, game thread lock wait %.1f ms\n",
                ms, present_ms, (long long)(c - rc), (long long)((b - rb) >> 10), (double)(q - rq) * f,
                s_gpu11.tex_uploads - texup, s_gpu11.surf_creates - surfs, s_ps_ready - ps, s_gpu11.draws - draws,
                (double)(e - ex) * f, (double)(w - qw) * f, (double)(k - cp) * f, (double)(lw - gw) * f);
    }
    rq = q; rb = b; rc = c; ex = e; qw = w; cp = k; gw = lw;
    draws = s_gpu11.draws; texup = s_gpu11.tex_uploads; surfs = s_gpu11.surf_creates; ps = s_ps_ready;
}

/* Called once per present, after the swap chain's Present returned. */
static void perf_on_present(int scale, float present_ms)
{
    LARGE_INTEGER now;
    float ms;
    if (!s_perf.freq.QuadPart) {
        QueryPerformanceFrequency(&s_perf.freq);
        QueryPerformanceCounter(&s_perf.last);
        s_perf.win_t0 = s_perf.pub_t0 = s_perf.last;
        s_perf.win_cpu0 = s_perf.pub_cpu0 = perf_process_cpu();
        return;
    }
    QueryPerformanceCounter(&now);
    ms = (float)((double)(now.QuadPart - s_perf.last.QuadPart) * 1000.0 / (double)s_perf.freq.QuadPart);
    s_perf.last = now;
    s_perf.ft[s_perf.head] = ms;
    s_perf.head = (s_perf.head + 1) % PERF_RING;
    s_perf.n++;
    s_perf.win_frames++; s_perf.win_ft_sum += ms; if (ms > s_perf.win_ft_max) s_perf.win_ft_max = ms;
    s_perf.pub_frames++; s_perf.pub_ft_sum += ms;
    perf_hitch(ms, present_ms);
    if (!s_perf.csv_tried) {
        const char *p = getenv("NFL2K5_FRAMETIME_CSV");
        s_perf.csv_tried = 1;
        if (p && *p && (s_perf.csv = fopen(p, "w")) != NULL)
            fprintf(s_perf.csv, "t_ms,frame_ms,gpu_ms,scale\n");
    }
    if (s_perf.csv)
        fprintf(s_perf.csv, "%.3f,%.3f,%.3f,%d\n",
                (double)now.QuadPart * 1000.0 / (double)s_perf.freq.QuadPart, ms, s_perf.gpu_ms, scale);

    /* Overlay numbers, once a second. */
    if (now.QuadPart - s_perf.pub_t0.QuadPart >= s_perf.freq.QuadPart) {
        ULONGLONG cpu = perf_process_cpu();
        float worst;
        s_perf.avg_fps = s_perf.pub_ft_sum > 0 ? (float)(s_perf.pub_frames * 1000.0 / s_perf.pub_ft_sum) : 0;
        s_perf.ft_avg = s_perf.pub_frames ? (float)(s_perf.pub_ft_sum / s_perf.pub_frames) : 0;
        s_perf.cpu_ms = s_perf.pub_frames ? (float)((cpu - s_perf.pub_cpu0) / 10000.0 / s_perf.pub_frames) : 0;
        perf_gpu_counter();
        /* GPU ms per frame = the 3D engine's busy share of each frame. */
        s_perf.gpu_ms = s_gpuctr.state == 1 ? s_gpuctr.util / 100.0f * s_perf.ft_avg : -1.0f;
        if (s_perf.gpu_ms >= 0) { s_perf.win_gpu_sum += s_perf.gpu_ms; s_perf.win_gpu_n++; }
        perf_percentiles(600, &s_perf.low1, &s_perf.low01, &worst);   /* the last ~10 s */
        s_perf.ft_max = worst;
        perf_memory();
        s_perf.pub_t0 = now; s_perf.pub_frames = 0; s_perf.pub_ft_sum = 0; s_perf.pub_cpu0 = cpu;
        if (s_perf.csv) fflush(s_perf.csv);
    }
    /* The log line, every 10 s, over that window. */
    if (now.QuadPart - s_perf.win_t0.QuadPart >= 10 * s_perf.freq.QuadPart) {
        ULONGLONG cpu = perf_process_cpu();
        double secs = (double)(now.QuadPart - s_perf.win_t0.QuadPart) / (double)s_perf.freq.QuadPart;
        float low1, low01, worst;
        perf_percentiles(s_perf.win_frames, &low1, &low01, &worst);   /* this window's frames only */
        fprintf(stderr, "  [BENCH] fps %.1f low1 %.1f low01 %.1f ft_avg %.2f ft_max %.1f gpu_ms %.2f gpu_util %.0f cpu_ms %.2f "
                        "ws_mb %u priv_mb %u vram_mb %u scale %d frames %u\n",
                s_perf.win_frames / secs, low1, low01, s_perf.win_frames ? s_perf.win_ft_sum / s_perf.win_frames : 0.0,
                s_perf.win_ft_max, s_perf.win_gpu_n ? s_perf.win_gpu_sum / s_perf.win_gpu_n : -1.0, s_gpuctr.util,
                s_perf.win_frames ? (cpu - s_perf.win_cpu0) / 10000.0 / s_perf.win_frames : 0.0,
                s_perf.ws_mb, s_perf.priv_mb, s_perf.vram_mb, scale, s_perf.win_frames);
        s_perf.win_t0 = now; s_perf.win_frames = 0; s_perf.win_ft_sum = 0; s_perf.win_ft_max = 0; s_perf.win_cpu0 = cpu;
        s_perf.win_gpu_sum = 0; s_perf.win_gpu_n = 0;
    }
}

/* Text for the Show FPS overlay: two lines. */
static int perf_overlay_text(char *l1, size_t n1, char *l2, size_t n2, int fps)
{
    snprintf(l1, n1, "%d FPS  %.1f ms", fps, s_perf.ft_avg);
    if (s_perf.gpu_ms >= 0)
        snprintf(l2, n2, "1%% low %.0f   GPU %.1f ms   CPU %.1f ms   VRAM %u MB",
                 s_perf.low1, s_perf.gpu_ms, s_perf.cpu_ms, s_perf.vram_mb);
    else
        snprintf(l2, n2, "1%% low %.0f   CPU %.1f ms   VRAM %u MB", s_perf.low1, s_perf.cpu_ms, s_perf.vram_mb);
    return 1;
}
