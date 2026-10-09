/* ── Controller button prompts (2026-10-09) ────────────────────────────────
 *
 * Included into nv2a_gpu_d3d11.inc.c. NFL 2K5 draws every button icon in its
 * menus and HUD from one 128x128 paletted sheet: a 4x4 grid of 32x32 cells
 *
 *     A      Y      X      B
 *     START  BACK   L-stk  (!)
 *     L-trg  R-trg  White  Black
 *     D-pad  L3     R3     R-stk
 *
 * When the title says PlayStation prompts are wanted (g_xbox_prompt_style,
 * set by its controller layer), the decoded sheet is recognised by a hash of
 * its pixels and the cells are repainted with PlayStation glyphs before it is
 * uploaded: Cross/Triangle/Square/Circle for A/Y/X/B, OPTIONS/SHARE for
 * START/BACK, L2/R2 for the triggers, R1/L1 for White/Black (this port maps
 * RB/LB there), L3/R3 for the stick clicks. The D-pad and stick cells stay.
 * The texture cache keys the sheet with the style (gpu_tex_hash), so a change
 * of controller re-uploads it.
 */

int (*g_xbox_prompt_style)(void);           /* 0 Xbox, 1 PlayStation; NULL = Xbox */

static int prompt_style(void) { return g_xbox_prompt_style ? g_xbox_prompt_style() : 0; }

static uint32_t prompt_hash(const uint32_t *p, uint32_t n)
{
    uint32_t h = 2166136261u, i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* The retail sheet's pixels (decoded BGRA) hash to this. */
#define PROMPT_SHEET_HASH 0xBD8DD7ADu   /* retail sheet at 0x813D7980 in menus, 2026-10-09 */

static uint32_t s_ps_sheet[128 * 128];
static int s_ps_sheet_built;

/* GDI-draw the PlayStation cells into s_ps_sheet (BGRA, alpha from coverage). */
static void prompt_build_ps(const uint32_t *orig)
{
    BITMAPINFO bi;
    void *bits = NULL;
    HDC dc;
    HBITMAP bmp, old;
    int i;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 128; bi.bmiHeader.biHeight = -128;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { if (dc) DeleteDC(dc); return; }
    old = (HBITMAP)SelectObject(dc, bmp);
    memcpy(bits, orig, sizeof s_ps_sheet);       /* keep the cells that do not change */
    {
        /* Face buttons: a dark disc with the coloured symbol. */
        static const struct { int cx, cy; int sym; COLORREF col; } face[4] = {
            { 0, 0, 0, RGB(124, 178, 232) },     /* A -> Cross (blue) */
            { 1, 0, 1, RGB(64, 226, 160) },      /* Y -> Triangle (green) */
            { 2, 0, 2, RGB(230, 140, 220) },     /* X -> Square (pink) */
            { 3, 0, 3, RGB(255, 102, 102) },     /* B -> Circle (red) */
        };
        HBRUSH disc = CreateSolidBrush(RGB(32, 34, 40));
        HPEN ring = CreatePen(PS_SOLID, 2, RGB(150, 150, 160));
        for (i = 0; i < 4; i++) {
            int x0 = face[i].cx * 32, y0 = face[i].cy * 32;
            HPEN sp = CreatePen(PS_SOLID, 3, face[i].col);
            HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
            RECT cell = { x0, y0, x0 + 32, y0 + 32 };
            HBRUSH clear = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(dc, &cell, clear);
            DeleteObject(clear);
            SelectObject(dc, disc); SelectObject(dc, ring);
            Ellipse(dc, x0 + 2, y0 + 2, x0 + 30, y0 + 30);
            SelectObject(dc, sp); SelectObject(dc, nb);
            switch (face[i].sym) {
            case 0: MoveToEx(dc, x0 + 10, y0 + 10, NULL); LineTo(dc, x0 + 22, y0 + 22);
                    MoveToEx(dc, x0 + 22, y0 + 10, NULL); LineTo(dc, x0 + 10, y0 + 22); break;
            case 1: { POINT t[4] = { { x0 + 16, y0 + 8 }, { x0 + 24, y0 + 22 }, { x0 + 8, y0 + 22 }, { x0 + 16, y0 + 8 } };
                      Polyline(dc, t, 4); } break;
            case 2: Rectangle(dc, x0 + 10, y0 + 10, x0 + 23, y0 + 23); break;
            case 3: Ellipse(dc, x0 + 9, y0 + 9, x0 + 24, y0 + 24); break;
            }
            DeleteObject(sp);
        }
        DeleteObject(disc); DeleteObject(ring);
    }
    {
        /* Labelled cells: a dark rounded plate with white text. */
        static const struct { int cx, cy; const char *t; } lab[8] = {
            { 0, 1, "OPTIONS" }, { 1, 1, "SHARE" }, { 0, 2, "L2" }, { 1, 2, "R2" },
            { 2, 2, "R1" }, { 3, 2, "L1" }, { 1, 3, "L3" }, { 2, 3, "R3" },
        };
        HFONT fbig = CreateFontA(15, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        HFONT fsmall = CreateFontA(9, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        HBRUSH plate = CreateSolidBrush(RGB(32, 34, 40));
        HPEN edge = CreatePen(PS_SOLID, 1, RGB(150, 150, 160));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        for (i = 0; i < 8; i++) {
            int x0 = lab[i].cx * 32, y0 = lab[i].cy * 32;
            RECT cell = { x0, y0, x0 + 32, y0 + 32 }, tr = { x0, y0 + 6, x0 + 32, y0 + 26 };
            HBRUSH clear = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(dc, &cell, clear);
            DeleteObject(clear);
            SelectObject(dc, plate); SelectObject(dc, edge);
            RoundRect(dc, x0 + 1, y0 + 5, x0 + 31, y0 + 27, 8, 8);
            SelectObject(dc, strlen(lab[i].t) > 2 ? fsmall : fbig);
            DrawTextA(dc, lab[i].t, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        DeleteObject(fbig); DeleteObject(fsmall); DeleteObject(plate); DeleteObject(edge);
    }
    GdiFlush();
    {
        /* Alpha: GDI leaves it 0. Cells drawn here are opaque where painted
         * and clear where black; untouched cells keep the original pixels. */
        static const unsigned char repainted[16] = { 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0 };
        uint32_t *p = (uint32_t *)bits;
        int x, y;
        for (y = 0; y < 128; y++)
            for (x = 0; x < 128; x++) {
                int cell = (y / 32) * 4 + x / 32;
                uint32_t c = p[y * 128 + x] & 0x00FFFFFFu;
                if (!repainted[cell]) { s_ps_sheet[y * 128 + x] = orig[y * 128 + x]; continue; }
                s_ps_sheet[y * 128 + x] = c ? (c | 0xFF000000u) : 0u;
            }
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    s_ps_sheet_built = 1;
    if (env_cached("RECOMP_PROMPT_LOG")) {           /* the sheet as built, for checking */
        FILE *f = fopen("logs\\prompt-sheet.bmp", "wb");
        if (f) {
            uint8_t hdr[54] = { 'B', 'M' };
            uint32_t size = 54 + 128 * 128 * 4;
            int32_t w = 128, h = -128;
            memcpy(hdr + 2, &size, 4); hdr[10] = 54; hdr[14] = 40;
            memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); hdr[26] = 1; hdr[28] = 32;
            fwrite(hdr, 1, 54, f);
            fwrite(s_ps_sheet, 4, 128 * 128, f);
            fclose(f);
        }
    }
}

/* After a 128x128 paletted texture is decoded into `pix` (BGRA). */
static void prompt_patch(uint32_t *pix, uint32_t addr)
{
    uint32_t h = prompt_hash(pix, 128 * 128);
    static int logged;
    if (env_cached("RECOMP_PROMPT_LOG") && logged++ < 64)
        fprintf(stderr, "  [PROMPTS] 128x128 paletted texture %08X hash %08X\n", addr, h);
    if (h != PROMPT_SHEET_HASH || PROMPT_SHEET_HASH == 0)
        return;
    if (env_cached("RECOMP_PROMPT_LOG"))
        fprintf(stderr, "  [PROMPTS] button sheet %08X: style %d\n", addr, prompt_style());
    if (prompt_style() != 1)
        return;
    if (!s_ps_sheet_built) prompt_build_ps(pix);
    if (s_ps_sheet_built) memcpy(pix, s_ps_sheet, sizeof s_ps_sheet);
    if (env_cached("RECOMP_PROMPT_LOG"))
        fprintf(stderr, "  [PROMPTS] button sheet %08X repainted (built %d)\n", addr, s_ps_sheet_built);
}
