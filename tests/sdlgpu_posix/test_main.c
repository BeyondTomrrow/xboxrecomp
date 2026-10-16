/* SDL3 GPU renderer, milestone 0: GPU clears and downloads against guest RAM.
 * Supplies the few executor symbols the renderer reads, then includes it.
 * Runs on any Vulkan device, Mesa lavapipe included. */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the executor state the renderer reads ---- */
static struct {
    uint32_t color_offset, pitch, clear_color;
    uint32_t clip_x, clip_w, clip_y, clip_h;
} s_gpu;
static uint32_t g_clear_rx = 0x0FFF0000u, g_clear_ry = 0x0FFF0000u;
static uint8_t *g_ram;                                   /* fake guest RAM */
static void *xbox_GetMemoryOffset(void) { return g_ram; }
static uint32_t dma_resolve(uint32_t offset) { return offset; }
static uint32_t surface_bpp(void) { return s_gpu.clip_w ? s_gpu.pitch / s_gpu.clip_w : 0; }
static void comb_unpack(uint32_t c, float v[4])
{
    v[0] = (float)((c >> 16) & 0xFF) / 255.0f; v[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    v[2] = (float)(c & 0xFF) / 255.0f;         v[3] = (float)(c >> 24) / 255.0f;
}

#include "kernel/sdlgpu/sdlgpu.inc.c"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void target(uint32_t off, uint32_t w, uint32_t h, uint32_t pitch)
{
    s_gpu.color_offset = off; s_gpu.pitch = pitch;
    s_gpu.clip_x = 0; s_gpu.clip_w = w; s_gpu.clip_y = 0; s_gpu.clip_h = h;
}

/* Every pixel of the w x h surface equals `want`; padding bytes untouched. */
static int surface_is(uint32_t off, uint32_t w, uint32_t h, uint32_t pitch, uint32_t want, uint8_t pad)
{
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = g_ram + off + (size_t)y * pitch;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px; memcpy(&px, row + x * 4, 4);
            if (px != want) { printf("  pixel (%u,%u) = %08X, want %08X\n", x, y, px, want); return 0; }
        }
        for (uint32_t b = w * 4; b < pitch; b++)
            if (row[b] != pad) { printf("  padding byte %u of row %u changed\n", b, y); return 0; }
    }
    return 1;
}

int main(void)
{
    setenv("RECOMP_SDLGPU", "1", 1);
    if (!SDL_Init(SDL_INIT_VIDEO)) { printf("SDL_Init: %s\n", SDL_GetError()); return 2; }
    g_ram = calloc(1, 16u << 20);

    /* 1. A full colour clear runs on the GPU and is not in guest RAM yet. */
    target(0x100000, 640, 480, 640 * 4);
    memset(g_ram + 0x100000, 0x5A, 640 * 480 * 4);
    s_gpu.clear_color = 0xFF2040C0u;
    CHECK(sg_clear(0xF0) == 1, "full colour clear not taken by the GPU");
    CHECK(s_sg.dev != NULL, "no device");
    CHECK(surface_is(0x100000, 640, 480, 640 * 4, 0x5A5A5A5Au, 0), "guest RAM changed before a flip");

    /* 2. The flip brings it back, exactly. */
    sg_flip();
    CHECK(surface_is(0x100000, 640, 480, 640 * 4, 0xFF2040C0u, 0), "flip did not deliver the clear colour");

    /* 3. Pitch wider than the row: padding survives. */
    target(0x400000, 320, 200, 1536);      /* like the title's 640 px in 3072-byte rows */
    memset(g_ram + 0x400000, 0xEE, 1536 * 200);
    s_gpu.clear_color = 0x80112233u;
    CHECK(sg_clear(0xF0) == 1, "padded surface clear not taken");
    sg_cpu_access();
    CHECK(surface_is(0x400000, 320, 200, 1536, 0x80112233u, 0xEE), "padded surface wrong after CPU access");

    /* 4. What milestone 0 must leave to the software path. */
    target(0x100000, 640, 480, 640 * 4);
    CHECK(sg_clear(0xF3) == 0, "colour+depth clear should fall back");
    CHECK(sg_clear(0x30) == 0, "channel-subset clear should fall back");
    g_clear_rx = (100u << 16) | 10u;                      /* x 10..100 */
    CHECK(sg_clear(0xF0) == 0, "partial-rectangle clear should fall back");
    g_clear_rx = 0x0FFF0000u;
    target(0x700000, 640, 480, 640 * 2);                  /* 16-bit target */
    CHECK(sg_clear(0xF0) == 0, "16-bit target should fall back");

    /* 5. Reuse: a second clear of the same surface, then a download. */
    target(0x100000, 640, 480, 640 * 4);
    s_gpu.clear_color = 0x00000000u;
    CHECK(sg_clear(0xF0) == 1, "second clear not taken");
    sg_flip();
    CHECK(surface_is(0x100000, 640, 480, 640 * 4, 0x00000000u, 0), "second clear wrong");
    {
        int live = 0;
        for (int i = 0; i < SG_MAX_SURFACES; i++) live += s_sg.surf[i].color != NULL;
        CHECK(live == 2, "expected 2 cached surfaces, have %d", live);
    }

    printf("%s (driver %s)\n", fails ? "FAILED" : "ALL PASS", s_sg.dev ? SDL_GetGPUDeviceDriver(s_sg.dev) : "none");
    SDL_Quit();
    return fails != 0;
}
