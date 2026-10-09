/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
uint32_t g_nv2a_pb_calls;   /* CALLs followed, see nv2a_pb_scan */

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;

/* The guest VA the *param* word for the method currently being dispatched to
 * nv2a_pb_exec_method was read from -- exposed so the executor (which only
 * gets subch/method/param, not where they came from) can log where a
 * specific command's data physically lives in guest RAM, without changing
 * that function's signature or its many call sites. */
uint32_t g_nv2a_pb_last_word_va;

static void note(uint32_t subch, uint32_t method)
{
    for (int i = 0; i < s_seen_count; i++) {
        if (s_seen[i].method == method && s_seen[i].subch == subch) {
            s_seen[i].count++;
            return;
        }
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

/* Set-but-"0" means off: tools/run-bringup.ps1 sets RECOMP_PB_SCAN=0 to
 * disable the survey, and a presence test turned that into "on", dumping a
 * few thousand [PB] lines every ten seconds. */
static int pb_scan_env_on(void)
{
    /* Cached: this ran getenv on every scan. */
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_PB_SCAN");
        on = v && *v && strcmp(v, "0") != 0;
    }
    return on;
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0)
        nv2a_pb_exec_report();
    if (!s_seen_count || !pb_scan_env_on())
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u unrecognised"
                    " -- %d distinct (subchannel, method) pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_unknown,
            s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

static int s_survey;

/* Asynchronous processing (RECOMP_GPU_ASYNC): the DMA_PUT register, so the
 * walk also stops where the title's PUT is now, as the GPU does. D3D patches
 * the jump ending the chunk it just submitted to point at its next chunk; a
 * walk that reads the patched jump runs through that (complete) chunk and
 * lands at the newer PUT, never at the PUT it was handed, and used to carry
 * on into stale ring memory (garbage surfaces, 2026-10-03). NULL when the
 * walk runs inside the kick (PUT cannot move meanwhile). g_nv2a_pb_walk_end
 * is where the last walk stopped. */
volatile uint32_t *g_nv2a_pb_live_put;
uint32_t g_nv2a_pb_walk_end;

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = start_va;
    uint32_t words = 0, jumps = 0, unknown = 0;
    /* Walk the way the GPU does: from the old PUT, following JUMPs and one
     * level of CALL/RETURN, until the walk arrives at the new PUT.
     *
     * The range [start, end) is not a flat buffer. NFL 2K5's DMA_PUT lives in
     * a small ring at physical 0x010Axxxx, and D3D reaches its 2.6 MB command
     * buffer (0x03470000.., where every draw is written) by JUMPing there
     * and back. This used to stop at the first JUMP and skipped CALLs, so the
     * executor saw only the ring's own few state methods -- 0 draws for the
     * whole game. Jump and call targets are physical, like DMA_PUT, and the
     * contiguous window is the physical view, hence the OR with its base.
     *
     * Termination: arriving at end_va, a word budget, and a jump budget (a
     * stream the title is still writing can contain a jump to itself). */
    const uint32_t base = start_va & ~0x0FFFFFFFu;
    uint32_t ret_va = 0;
    int in_sub = 0;

    g_nv2a_pb_walk_end = end_va;          /* an early return walked nothing */
    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    s_survey = pb_scan_env_on();
    if (!(pb_scan_env_on() || s_exec_enabled) || end_va == start_va)
        return;

    /* Breadcrumbs for [PBDESYNC]: the last control-flow words and large
     * method headers, with where they were read. */
    uint32_t trail_va[16], trail_w[16];
    unsigned trail_n = 0;
#define PB_TRAIL(a_, w_) do { trail_va[trail_n & 15] = (a_); trail_w[trail_n & 15] = (w_); trail_n++; } while (0)
    while (va != end_va && words < 0x100000u && jumps < 4096u) {
        uint32_t w;
        if (g_nv2a_pb_live_put && va == (base | (*g_nv2a_pb_live_put & 0x0FFFFFFCu)))
            break;                            /* reached the title's current PUT */
        /* A jump decoded from a word the title has not finished writing can
         * point anywhere; physical memory is 64 MB. */
        if (va - base >= 64u * 1024u * 1024u) { unknown++; break; }
        w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            PB_TRAIL(va - 4, w);
            jumps++;
            va = base | (((w & 3u) == 1u ? w : (w & 0x1FFFFFFFu)) & 0x0FFFFFFCu);
            continue;
        }
        if ((w & 3u) == 2u) {                 /* call */
            if (in_sub) { unknown++; break; } /* no nesting on the NV2A */
            PB_TRAIL(va - 4, w);
            g_nv2a_pb_calls++;
            ret_va = va;
            in_sub = 1;
            va = base | (w & 0x0FFFFFFCu);
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) { /* return */
            PB_TRAIL(va - 4, w);
            if (in_sub) {
                va = ret_va;
                in_sub = 0;
            }
            continue;
        }
        /* A method header is 0x00000000 (increasing) or 0x40000000
         * (non-increasing) in its top three bits, like xemu's pusher checks
         * with (w & 0xE0030003). Anything else is reserved: taking it as a
         * method walked garbage and scrambled GPU state in-game. Stop the
         * segment, and say where it happened. */
        if ((w & 0x00030003u) == 0u && (w & 0xE0000000u) != 0u && (w & 0xE0000000u) != 0x40000000u) {
            static unsigned shown;
            unknown++;
            if (shown++ < 6) {
                uint32_t k;
                fprintf(stderr, "  [PBDESYNC] reserved header %08X at va %08X (segment %08X..%08X, words %u, sub %d)\n",
                        w, va - 4, start_va, end_va, words, in_sub);
                for (k = trail_n > 16 ? trail_n - 16 : 0; k < trail_n; k++)
                    fprintf(stderr, "  [PBDESYNC]   trail %08X: %08X" "%s", trail_va[k & 15], trail_w[k & 15], "\n");
                for (k = 0; k < 12; k++) {
                    uint32_t at = va - 4 - 32 + k * 4;
                    if (at - base < 64u * 1024u * 1024u)
                        fprintf(stderr, "  [PBDESYNC]   %08X: %08X\n", at, *(const uint32_t *)(mem + at));
                }
            }
            break;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            if (count > 64)
                PB_TRAIL(va - 4, w);
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            /* Data runs (constants, vertices, indices) in one call. */
            if (s_exec_enabled && !s_survey && count >= 2) {
                extern uint32_t nv2a_pb_exec_run(uint32_t subch, uint32_t method, int noninc,
                                                 const uint32_t *w, uint32_t n);
                uint32_t n = count, done;
                if (end_va >= va && (end_va - va) / 4u < n)
                    n = (end_va - va) / 4u;
                if ((va - base) + n * 4u > 64u * 1024u * 1024u)
                    n = (64u * 1024u * 1024u - (va - base)) / 4u;
                done = n >= 2 ? nv2a_pb_exec_run(subch, method, noninc, (const uint32_t *)(mem + va), n) : 0;
                if (done) {
                    g_nv2a_pb_last_word_va = va + (done - 1) * 4u;
                    va += done * 4u;
                    words += done;
                    if (done == count)
                        continue;
                    /* Cut short by end_va: the per-word loop below sees the
                     * rest of the header's count, as before. */
                    method = noninc ? method : method + done * 4u;
                    count -= done;
                }
            }

            for (uint32_t i = 0; i < count && va != end_va && va - base < 64u * 1024u * 1024u; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                /* The method inventory is a linear search per word: only
                 * when the survey was asked for (it was ~30% of a frame). */
                if (s_survey)
                    note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled) {
                    g_nv2a_pb_last_word_va = va;
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                }
                va += 4;
                words++;
            }
            continue;
        }
        unknown++;
    }

    g_nv2a_pb_walk_end = va;
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

/* ---- asynchronous processing: capture at kick, execute later ------------
 *
 * RECOMP_GPU_ASYNC runs the commands on a render thread. Walking guest memory
 * later is racy: D3D patches the jump ending a submitted chunk to point at
 * the next one, and reuses ring and chunk memory once GET says so, so a late
 * walk followed patched jumps into chunks still being written (garbage
 * surfaces, 2026-10-03). Instead the kick copies its segment -- following
 * jumps, calls and returns exactly as the synchronous walk reads them, at
 * the same moment -- into a linear list of method headers and their data,
 * and the render thread executes that copy. Data the commands only point at
 * (vertex arrays, textures) is still read when drawn; D3D's fences guard it
 * as on hardware. */

/* Copy [start_va, end_va) as headers + data into *buf (grown with realloc).
 * Returns the word count; *ok is 0 when the walk lost the stream (reserved
 * header, budget), like the [PBDESYNC] stop in nv2a_pb_scan. */
uint32_t nv2a_pb_capture(uint32_t start_va, uint32_t end_va, uint32_t **buf, uint32_t *cap, int *ok)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint32_t base = start_va & ~0x0FFFFFFFu;
    uint32_t va = start_va, n = 0, words = 0, jumps = 0, ret_va = 0;
    int in_sub = 0;
    *ok = 1;
#define PB_NEED(k_) do { if (n + (k_) > *cap) { \
        uint32_t nc = *cap ? *cap : 4096; while (nc < n + (k_)) nc *= 2; \
        uint32_t *nb = (uint32_t *)realloc(*buf, (size_t)nc * 4); \
        if (!nb) { *ok = 0; return n; } *buf = nb; *cap = nc; } } while (0)
    while (va != end_va && words < 0x100000u && jumps < 4096u) {
        uint32_t w;
        if (va - base >= 64u * 1024u * 1024u) { *ok = 0; break; }
        w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;
        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            jumps++;
            va = base | (((w & 3u) == 1u ? w : (w & 0x1FFFFFFFu)) & 0x0FFFFFFCu);
            continue;
        }
        if ((w & 3u) == 2u) {
            if (in_sub) { *ok = 0; break; }
            ret_va = va;
            in_sub = 1;
            va = base | (w & 0x0FFFFFFCu);
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {
            if (in_sub) { va = ret_va; in_sub = 0; }
            continue;
        }
        if ((w & 0x00030003u) == 0u && (w & 0xE0000000u) != 0u && (w & 0xE0000000u) != 0x40000000u) {
            *ok = 0;
            break;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count = (w >> 18) & 0x7FFu, k = 0, hdr;
            PB_NEED(1 + count);
            hdr = n++;
            while (k < count && va != end_va && va - base < 64u * 1024u * 1024u) {
                /* Up to the end of the segment or of physical memory. */
                uint32_t run = count - k, room;
                if (end_va >= va && (end_va - va) / 4u < run) run = (end_va - va) / 4u;
                room = (64u * 1024u * 1024u - (va - base)) / 4u;
                if (room < run) run = room;
                if (!run) break;
                memcpy(*buf + n, mem + va, (size_t)run * 4);
                n += run; va += run * 4u; words += run; k += run;
            }
            (*buf)[hdr] = (w & ~(0x7FFu << 18)) | (k << 18);
            continue;
        }
        *ok = 0;
        break;
    }
#undef PB_NEED
    if (words >= 0x100000u || jumps >= 4096u) *ok = 0;
    g_nv2a_pb_walk_end = va;
    return n;
}

/* Execute a captured list (render thread). */
void nv2a_pb_exec_linear(const uint32_t *b, uint32_t n)
{
    extern uint32_t nv2a_pb_exec_run(uint32_t subch, uint32_t method, int noninc,
                                     const uint32_t *w, uint32_t n);
    uint32_t i = 0;
    while (i < n) {
        uint32_t w = b[i++];
        uint32_t count = (w >> 18) & 0x7FFu;
        uint32_t subch = (w >> 13) & 7u;
        uint32_t method = w & 0x1FFCu;
        int noninc = (w & 0xE0000000u) == 0x40000000u;
        uint32_t k = 0;
        if (count > n - i) count = n - i;
        g_nv2a_pb_last_word_va = 0;
        if (count >= 2) {
            k = nv2a_pb_exec_run(subch, method, noninc, b + i, count);
        }
        for (; k < count; k++)
            nv2a_pb_exec_method(subch, noninc ? method : method + k * 4u, b[i + k]);
        i += count;
    }
}
