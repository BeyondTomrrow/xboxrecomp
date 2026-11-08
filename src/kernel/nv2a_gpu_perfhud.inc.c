/* ── Performance overlay (2026-10-31) ─────────────────────────────────────
 *
 * Included into nv2a_gpu_present.inc.c, before pr_draw_hud.
 *
 * The "Performance Overlay" video setting (show_fps: 0 Off, 1 FPS only,
 * 2 Full) draws a small panel in the top-left corner as HUD layer 2: a
 * premultiplied BGRA image drawn on the CPU with a built-in 5x7 font, so the
 * same code shows on Direct3D 11, the SDL_GPU backends (Direct3D 12, Vulkan)
 * and Android -- there is no GDI or DirectWrite there. The image is redrawn
 * four times a second, not every frame; between redraws the layer reuses
 * the uploaded texture.
 *
 * Full mode: FPS and 1% low, frame time with a graph of the last frames,
 * game-thread time (guest code holding the guest CPU lock) and render-thread
 * time (push-buffer execution) per frame, GPU time where it can be measured
 * (Windows: the 3D engine's busy share; Android: Adreno/Mali busy files read
 * by src/main.c), internal and output resolution with the dynamic-resolution
 * state, process CPU use, and on Android the busiest threads, battery level,
 * drain and temperature (src/main.c fills g_xbox_device_stats). */

/* Filled by the platform layer (src/main.c on Android); fields it does not
 * know stay at their "unknown" values. Same layout declared there. */
typedef struct {
    volatile LONG seq;              /* bumped after each update */
    int   valid;
    char  top[2][16];               /* the two busiest threads in the last sample */
    float top_pct[2];               /* percent of one core */
    int   ncores;
    int   batt_pct;                 /* -1 unknown */
    int   batt_ma;                  /* current, mA; negative while discharging; 0 unknown */
    float batt_drain_pct_h;         /* percent per hour, positive = draining; -1000 unknown */
    int   charging;
    float batt_temp_c;              /* -1000 unknown */
    float soc_temp_c;               /* a CPU/SoC thermal zone, -1000 unknown */
    int   thermal_status;           /* PowerManager THERMAL_STATUS_*: 0 none .. 6 shutdown; -1 unknown */
    float thermal_headroom;         /* PowerManager.getThermalHeadroom: 1.0 = throttling; -1 unknown */
    float gpu_pct;                  /* -1 unknown */
} XboxDeviceStats;
XboxDeviceStats g_xbox_device_stats = { 0, 0, { "", "" }, { 0, 0 }, 0, -1, 0, -1000.0f, 0, -1000.0f, -1000.0f, -1, -1.0f, -1.0f };
/* The overlay's mode, for samplers that only need to run while it shows. */
volatile int g_xbox_perf_overlay;

LONGLONG xbox_ggl_main_held_qpc(void);
int xbox_VideoFpsLimit(void);
static int vs_battery_mode(void);
extern volatile int g_xbox_heat_level;

/* 5x7 ASCII 0x20..0x7E, one byte per column, bit 0 at the top. */
static const uint8_t k_ph_font[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x02,0x01,0x02,0x04,0x02},
};

#define PH_COLS 34                   /* characters per line in Full mode */
#define PH_GREEN  0xFF78F078u
#define PH_YELLOW 0xFFF0D850u
#define PH_RED    0xFFFF6060u
#define PH_WHITE  0xFFE8ECF0u
#define PH_GREY   0xFFA0A8B4u
#define PH_LABEL  0xFF8CC8FFu

static struct {
    uint32_t *img;
    int w, h, cap;                  /* pixels; capacity in pixels */
    int s;                          /* font pixel size */
    int mode, bb_w, bb_h;
    LONGLONG last_qpc;              /* last redraw */
    /* the previous sample, for per-interval rates */
    LONGLONG t0, held0, exec0;
    unsigned frames0;
    ULONGLONG cpu0;
    int have0;
    /* what the last sample measured */
    float fps, ft, game_ms, render_ms, cpu_pct;
} s_ph;

static void ph_rect(int x0, int y0, int w, int h, uint32_t c)
{
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > s_ph.w) w = s_ph.w - x0;
    if (y0 + h > s_ph.h) h = s_ph.h - y0;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            s_ph.img[y * s_ph.w + x] = c;
}

/* One line of text at (x, y), top-left, with a dark drop shadow. Returns the
 * x after it. */
static int ph_text(int x, int y, const char *t, uint32_t col)
{
    int s = s_ph.s, sh = s > 1 ? s / 2 : 1;
    for (; *t; t++, x += 6 * s) {
        unsigned char ch = (unsigned char)*t;
        const uint8_t *g;
        int cx, cy;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        g = k_ph_font[ch - 0x20];
        for (cx = 0; cx < 5; cx++)
            for (cy = 0; cy < 8; cy++)
                if (g[cx] & (1u << cy))
                    ph_rect(x + cx * s + sh, y + cy * s + sh, s, s, 0xFF000000u);
        for (cx = 0; cx < 5; cx++)
            for (cy = 0; cy < 8; cy++)
                if (g[cx] & (1u << cy))
                    ph_rect(x + cx * s, y + cy * s, s, s, col);
    }
    return x;
}

static float ph_target_ms(void)
{
    int cap = xbox_VideoFpsLimit();
    return cap > 0 ? 1000.0f / (float)cap : 1000.0f / 60.0f;
}

static uint32_t ph_rate_colour(float fps)
{
    float want = 1000.0f / ph_target_ms();
    return fps >= want * 0.95f ? PH_GREEN : fps >= want * 0.75f ? PH_YELLOW : PH_RED;
}

/* The frame-time graph: one bar per frame, newest on the right, with lines
 * at the cap's frame time and at twice it. */
static void ph_graph(int x0, int y0, int w, int h)
{
    int s = s_ph.s, bw = s > 1 ? s / 2 + 1 : 1, nbars = w / bw, i;
    unsigned have = s_perf.n < PERF_RING ? s_perf.n : PERF_RING;
    float target = ph_target_ms(), top = target * 3.0f;
    ph_rect(x0, y0, w, h, 0xD0000000u);
    if ((unsigned)nbars > have) nbars = (int)have;
    for (i = 0; i < nbars; i++) {
        float ms = s_perf.ft[(s_perf.head + PERF_RING - (unsigned)nbars + (unsigned)i) % PERF_RING];
        int bh = (int)(ms / top * (float)h + 0.5f);
        uint32_t c = ms <= target * 1.15f ? PH_GREEN : ms <= target * 2.1f ? PH_YELLOW : PH_RED;
        if (bh > h) bh = h;
        if (bh < 1) bh = 1;
        ph_rect(x0 + w - (nbars - i) * bw, y0 + h - bh, bw, bh, c);
    }
    ph_rect(x0, y0 + h - (int)((float)h / 3.0f), w, bw > 1 ? bw - 1 : 1, 0xC0C0C0C0u);          /* the cap */
    ph_rect(x0, y0 + h - (int)((float)h * 2.0f / 3.0f), w, bw > 1 ? bw - 1 : 1, 0x60606060u);   /* twice it */
}

/* Rates over the time since the last redraw. */
static void ph_sample(LONGLONG now)
{
    extern volatile LONG64 g_pb_exec_qpc;
    LONGLONG held = xbox_ggl_main_held_qpc(), exec = g_pb_exec_qpc;
    unsigned frames = s_perf.n;
    ULONGLONG cpu = perf_process_cpu();
    if (s_ph.have0 && now > s_ph.t0 && s_perf.freq.QuadPart) {
        double f = (double)s_perf.freq.QuadPart, dt = (double)(now - s_ph.t0) / f;
        unsigned df = frames - s_ph.frames0;
        s_ph.fps = df ? (float)(df / dt) : 0.0f;
        s_ph.ft = df ? (float)(dt * 1000.0 / df) : 0.0f;
        s_ph.game_ms = df && held >= s_ph.held0 ? (float)((double)(held - s_ph.held0) * 1000.0 / f / df) : -1.0f;
        s_ph.render_ms = df && exec > s_ph.exec0 ? (float)((double)(exec - s_ph.exec0) * 1000.0 / f / df) : -1.0f;
        s_ph.cpu_pct = cpu >= s_ph.cpu0 ? (float)((double)(cpu - s_ph.cpu0) / 100000.0 / dt) : -1.0f;  /* 100 ns units */
    }
    s_ph.t0 = now; s_ph.held0 = held; s_ph.exec0 = exec; s_ph.frames0 = frames; s_ph.cpu0 = cpu; s_ph.have0 = 1;
}

static void ph_build(void)
{
    char ln[16][64];
    uint32_t col[16];
    int n = 0, i, s, pad, lh, graph_h = 0, graph_at = -1, w, h, y;
    XboxDeviceStats ds = g_xbox_device_stats;
    float fps = s_ph.fps > 0 ? s_ph.fps : s_perf.avg_fps;
    s = s_ph.s;
    if (s_ph.mode == 1) {
        snprintf(ln[n], 64, "%.0f FPS  %.1f ms  1%% low %.0f", fps, s_ph.ft, s_perf.low1);
        col[n++] = ph_rate_colour(fps);
    } else {
        int cap = xbox_VideoFpsLimit();
        unsigned sc = g_render_scale ? g_render_scale : 1u;
        float worst = 0.0f;
        unsigned k, have = s_perf.n < PERF_RING ? s_perf.n : PERF_RING;
        for (k = 0; k < have && k < 120; k++) {
            float v = s_perf.ft[(s_perf.head + PERF_RING - 1 - k) % PERF_RING];
            if (v > worst) worst = v;
        }
        if (cap > 0) snprintf(ln[n], 64, "%.0f FPS  1%% low %.0f  cap %d", fps, s_perf.low1, cap);
        else snprintf(ln[n], 64, "%.0f FPS  1%% low %.0f", fps, s_perf.low1);
        col[n++] = ph_rate_colour(fps);
        snprintf(ln[n], 64, "Frame %.1f ms  worst %.1f", s_ph.ft, worst);
        col[n++] = PH_WHITE;
        graph_at = n;                /* the graph goes after the frame line */
        if (s_ph.game_ms >= 0 && s_ph.render_ms >= 0)
            snprintf(ln[n], 64, "Game %.1f ms  Render %.1f ms", s_ph.game_ms, s_ph.render_ms);
        else if (s_ph.game_ms >= 0)
            snprintf(ln[n], 64, "Game %.1f ms  Render (same)", s_ph.game_ms);
        else
            snprintf(ln[n], 64, "Game --  Render --");
        col[n++] = PH_WHITE;
        if (s_perf.gpu_ms >= 0)
            snprintf(ln[n], 64, "GPU %.1f ms  busy %.0f%%", s_perf.gpu_ms, s_gpuctr.util);
        else if (ds.gpu_pct >= 0)
            snprintf(ln[n], 64, "GPU busy %.0f%%", ds.gpu_pct);
        else
            snprintf(ln[n], 64, "GPU time n/a");
        col[n++] = PH_WHITE;
        if (s_ph.cpu_pct >= 0) {
            int cores = ds.ncores > 0 ? ds.ncores : SDL_GetNumLogicalCPUCores();
            snprintf(ln[n], 64, "CPU %.0f%% of %d%%", s_ph.cpu_pct, cores * 100);
            col[n++] = PH_WHITE;
        }
        if (ds.valid && ds.top[0][0]) {
            if (ds.top[1][0])
                snprintf(ln[n], 64, " %.10s %.0f%%  %.10s %.0f%%", ds.top[0], ds.top_pct[0], ds.top[1], ds.top_pct[1]);
            else
                snprintf(ln[n], 64, " %.12s %.0f%%", ds.top[0], ds.top_pct[0]);
            col[n++] = PH_GREY;
        }
        {
            const char *how = vs_battery_mode() ? "battery" : g_xbox_heat_level ? "heat" :
                              s_vs.dynres ? "auto" : "fixed";
            if ((s_vs.upscaler || s_vs.framegen) && gxs_fsr_available()) how = "FSR";
            snprintf(ln[n], 64, "Res %ux %ux%u  %s, set %dx", sc, 720u * sc, 480u * sc, how, s_vs.scale);
            col[n++] = PH_WHITE;
        }
        snprintf(ln[n], 64, "Out %ux%u  %s", s_pr.bb_w, s_pr.bb_h, gx_backend_name());
        col[n++] = PH_GREY;
        if (ds.valid && ds.batt_pct >= 0) {
            char drain[24] = "", cur[16] = "";
            if (ds.charging) snprintf(drain, sizeof drain, " charging");
            else if (ds.batt_drain_pct_h > -999.0f) snprintf(drain, sizeof drain, " -%.1f%%/h", ds.batt_drain_pct_h);
            if (ds.batt_ma) snprintf(cur, sizeof cur, " %d mA", ds.batt_ma);
            snprintf(ln[n], 64, "Battery %d%%%s%s", ds.batt_pct, drain, cur);
            col[n++] = ds.batt_pct <= 15 && !ds.charging ? PH_RED : PH_WHITE;
        }
        if (ds.valid && (ds.batt_temp_c > -999.0f || ds.soc_temp_c > -999.0f || ds.thermal_status >= 0)) {
            static const char *const k_th[] = { "ok", "light", "moderate", "severe", "critical", "emergency", "shutdown" };
            char a[20] = "", b[20] = "", c[24] = "";
            uint32_t tc = PH_WHITE;
            if (ds.batt_temp_c > -999.0f) snprintf(a, sizeof a, " batt %.1fC", ds.batt_temp_c);
            if (ds.soc_temp_c > -999.0f) snprintf(b, sizeof b, " soc %.0fC", ds.soc_temp_c);
            if (ds.thermal_status >= 0 && ds.thermal_status <= 6) {
                snprintf(c, sizeof c, " %s", k_th[ds.thermal_status]);
                tc = ds.thermal_status >= 3 ? PH_RED : ds.thermal_status >= 1 ? PH_YELLOW : PH_WHITE;
            }
            snprintf(ln[n], 64, "Temp%s%s%s", a, b, c);
            col[n++] = tc;
        }
    }
    pad = 3 * s;
    lh = 10 * s;
    {
        int cols = 0;
        for (i = 0; i < n; i++) { int l = (int)strlen(ln[i]); if (l > cols) cols = l; }
        if (s_ph.mode == 2 && cols < PH_COLS) cols = PH_COLS;
        w = cols * 6 * s + 2 * pad;
    }
    if (graph_at >= 0) graph_h = 16 * s + 2 * s;
    h = n * lh + graph_h + 2 * pad - 2 * s;
    if (w * h > s_ph.cap) {
        free(s_ph.img);
        s_ph.img = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
        s_ph.cap = s_ph.img ? w * h : 0;
        if (!s_ph.img) { s_ph.w = s_ph.h = 0; return; }
    }
    s_ph.w = w; s_ph.h = h;
    for (i = 0; i < w * h; i++) s_ph.img[i] = 0xA0000000u;    /* premultiplied black, 63% */
    y = pad;
    for (i = 0; i < n; i++) {
        if (i == graph_at) {
            ph_graph(pad, y, w - 2 * pad, 16 * s);
            y += graph_h;
        }
        ph_text(pad, y, ln[i], col[i]);
        y += lh;
    }
}

/* HUD layer 2's callback: the panel, rebuilt every 250 ms. */
static int perfhud_frame(const XboxHudFrame *f, XboxHudImage *img)
{
    LARGE_INTEGER now;
    int mode = s_vs.show_fps > 2 ? 2 : s_vs.show_fps, s;
    {   /* NFL2K5_PERF_OVERLAY=0/1/2 overrides the setting (tests, captures) */
        static int forced = -2;
        if (forced == -2) { const char *e = getenv("NFL2K5_PERF_OVERLAY"); forced = e && *e ? atoi(e) : -1; }
        if (forced >= 0) mode = forced > 2 ? 2 : forced;
    }
    g_xbox_perf_overlay = mode;
    if (!mode || s_menu_open || f->bb_w <= 0 || f->bb_h <= 0 || !s_perf.freq.QuadPart)
        return 0;
#ifdef __ANDROID__
    s = (f->bb_h + 180) / 360;       /* a phone: 3 at 1080 lines */
#else
    s = (f->bb_h + 270) / 540;       /* 2 at 1080p, 3 at 1440p, 4 at 2160p */
#endif
    if (s < 1) s = 1;
    if (s > 6) s = 6;
    QueryPerformanceCounter(&now);
    img->changed = 0;
    if (!s_ph.img || mode != s_ph.mode || s != s_ph.s || f->bb_w != s_ph.bb_w || f->bb_h != s_ph.bb_h ||
            now.QuadPart - s_ph.last_qpc >= s_perf.freq.QuadPart / 4) {
        ph_sample(now.QuadPart);
        s_ph.mode = mode; s_ph.s = s; s_ph.bb_w = f->bb_w; s_ph.bb_h = f->bb_h;
        s_ph.last_qpc = now.QuadPart;
        ph_build();
        img->changed = 1;
    }
    if (!s_ph.img || s_ph.w <= 0) return 0;
    img->pixels = s_ph.img;
    img->w = s_ph.w; img->h = s_ph.h; img->stride = s_ph.w * 4;
    img->x = 4 * s;
#ifdef __ANDROID__
    img->y = f->bb_h * 18 / 100;     /* below the touch controls' shoulder buttons */
#else
    img->y = 4 * s;
#endif
    return 1;
}
