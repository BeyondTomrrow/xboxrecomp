/* Data packs: the title's data folder served from one .xbpak (2026-11-01).
 *
 * The Xbox (Dev Mode) build has a few GB of app storage; the data folder
 * (vc_53450030, 5.8 GB) is packed by tools/xbpack into a random-access,
 * block-compressed file, optionally without the movies (xbpak_format.h).
 * When <game folder>\vc_53450030.xbpak exists and the folder's files do not,
 * an open of one of them gets a handle to the pack, and kernel_file.c /
 * kernel_overlay.c read through here: same names, sizes and offsets as the
 * original files, so the title and a mod pack's overlay work unchanged.
 *
 * Decompressed blocks are kept in a cache (NFL2K5_XBPAK_CACHE_MB, default
 * 256): a compressible region is one 16 MB block, unpacked when a scene
 * loads (~50-90 ms each on a desktop CPU); dense data (audio, read at random
 * during play) is stored raw or in 256 KB blocks (~1 ms).
 */
#if !defined(_WIN32)
#include "kernel.h"          /* the Windows types (win32_compat on POSIX hosts) */
#endif
#include "kernel_pack.h"

#if defined(XBOX_HAVE_ZSTD)
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>          /* pread */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zstd.h"
#include "xbpak_format.h"

/* Paths here are Xbox-style WCHAR (16-bit) on every host; POSIX wchar_t is
 * 32-bit, so the C library's wide string functions do not apply there. */
static size_t w16len(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static int w16icmp(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        WCHAR x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (WCHAR)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (WCHAR)(y + 32);
        if (x != y || !x) return (int)x - (int)y;
    }
}
#ifndef _WIN32
#define InterlockedIncrement64(p) InterlockedAdd64((p), 1)
#ifndef FILE_FLAG_RANDOM_ACCESS
#define FILE_FLAG_RANDOM_ACCESS 0x10000000u   /* a hint; win32_compat ignores it */
#endif
#endif

typedef struct {
    WCHAR rel[64];                  /* "vc_53450030\3" */
    uint64_t size;
    uint32_t first_block, nblocks;
} PakFile;

typedef struct {
    uint32_t block;                 /* global block index, or ~0 */
    uint8_t *data;
    uint32_t cap;
    uint64_t used;                  /* LRU stamp */
} CacheSlot;

static WCHAR s_pak_path[MAX_PATH];
static HANDLE s_pak = INVALID_HANDLE_VALUE;   /* shared, positional reads only */
static XbpakHeader s_hdr;
static PakFile *s_pfiles;
static XbpakBlock *s_blocks;
static uint32_t s_drop_flags;

#define PAK_SLOTS 32
static CacheSlot s_cache[PAK_SLOTS];
static uint64_t s_cache_budget, s_cache_bytes, s_stamp;
static SRWLOCK s_cache_lock = SRWLOCK_INIT;
static ZSTD_DCtx *s_dctx;
static uint8_t *s_scratch;

#define PAK_HANDLES 256
static struct { HANDLE h; uint32_t file; } s_open[PAK_HANDLES];
static SRWLOCK s_open_lock = SRWLOCK_INIT;

static volatile LONG64 s_stat_hits, s_stat_misses, s_stat_unpacked;
static LONG64 s_stat_us;                        /* reading + decompressing missed blocks */
static int s_log_misses;                        /* NFL2K5_XBPAK_LOG=1: one line per unpacked block */
static ULONGLONG s_load_ms;

static int pread_all(HANDLE h, void *buf, uint32_t n, uint64_t off)
{
    uint8_t *p = (uint8_t *)buf;
#ifndef _WIN32
    int fd = w32_handle_fd(h);
    if (fd < 0) return 0;
#endif
    while (n) {
#ifdef _WIN32
        OVERLAPPED ov;
        DWORD got = 0;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
        if (!ReadFile(h, p, n, &got, &ov) || !got) return 0;
#else
        ssize_t got = pread(fd, p, n, (off_t)off);   /* positional: threads share s_pak */
        if (got <= 0) return 0;
#endif
        p += got; n -= (uint32_t)got; off += (uint64_t)got;
    }
    return 1;
}

int xbox_pack_load(const char *game_dir)
{
    char path[MAX_PATH];
    uint8_t *ix, *q, *end;
    uint32_t i;
    const char *mb = getenv("NFL2K5_XBPAK_CACHE_MB");
    if (s_pfiles) return 1;
    if (getenv("NFL2K5_NO_XBPAK")) return 0;
    snprintf(path, sizeof path, "%s/vc_53450030.xbpak", game_dir);
#ifdef _WIN32
    for (char *c = path; *c; c++) if (*c == '/') *c = '\\';
#endif
    MultiByteToWideChar(CP_UTF8, 0, path, -1, s_pak_path, MAX_PATH);
    s_pak = CreateFileW(s_pak_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (s_pak == INVALID_HANDLE_VALUE) return 0;
    if (!pread_all(s_pak, &s_hdr, sizeof s_hdr, 0) || memcmp(s_hdr.magic, XBPAK_MAGIC, 8)
            || s_hdr.version != XBPAK_VERSION || !s_hdr.nfiles || s_hdr.nfiles > 64
            || s_hdr.index_size > (64u << 20)) {
        fprintf(stderr, "  [XBPAK] %s is not a data pack this build reads\n", path);
        goto fail;
    }
    ix = (uint8_t *)malloc((size_t)s_hdr.index_size);
    if (!ix || !pread_all(s_pak, ix, (uint32_t)s_hdr.index_size, s_hdr.index_off)) {
        fprintf(stderr, "  [XBPAK] %s: index unreadable\n", path);
        free(ix);
        goto fail;
    }
    q = ix; end = ix + s_hdr.index_size;
    s_pfiles = (PakFile *)calloc(s_hdr.nfiles, sizeof *s_pfiles);
    for (i = 0; i < s_hdr.nfiles; i++) {
        uint16_t nl;
        uint32_t nd;
        char name[64];
        if (q + 2 > end) goto bad;
        /* memcpy: the index is packed, and 32-bit ARM faults on unaligned
         * 64-bit loads */
        memcpy(&nl, q, 2); q += 2;
        if (nl >= sizeof name || q + nl + 32 > end) goto bad;
        memcpy(name, q, nl); name[nl] = 0; q += nl;
        for (char *c = name; *c; c++) if (*c == '/') *c = '\\';
        MultiByteToWideChar(CP_UTF8, 0, name, -1, s_pfiles[i].rel, 64);
        memcpy(&s_pfiles[i].size, q, 8); q += 8;
        q += 8;                                         /* xxh64: checked by the tool */
        memcpy(&s_pfiles[i].first_block, q, 4); q += 4;
        memcpy(&s_pfiles[i].nblocks, q, 4); q += 4;
        memcpy(&nd, q, 4); q += 4;
        if (q + (size_t)nd * 20 > end) goto bad;
        q += (size_t)nd * 20;                           /* dropped ranges: the header flags say what */
        if ((uint64_t)s_pfiles[i].first_block + s_pfiles[i].nblocks > s_hdr.nblocks) goto bad;
    }
    if (q + (size_t)s_hdr.nblocks * sizeof(XbpakBlock) > end) goto bad;
    /* an aligned copy (see above); the index buffer itself stays allocated */
    s_blocks = (XbpakBlock *)malloc((size_t)s_hdr.nblocks * sizeof(XbpakBlock));
    if (!s_blocks) goto bad;
    memcpy(s_blocks, q, (size_t)s_hdr.nblocks * sizeof(XbpakBlock));
    s_drop_flags = s_hdr.drop_flags;
    /* 256 MB: sixteen 16 MB regions. At 96 MB a game's scene loads unpacked
     * almost half their regions twice (63 unpacks of 35 regions, PC test). */
    s_cache_budget = (uint64_t)(mb && *mb ? atoi(mb) : 256) << 20;
    if (s_cache_budget < 2ull * s_hdr.max_ulen) s_cache_budget = 2ull * s_hdr.max_ulen;
    for (i = 0; i < PAK_SLOTS; i++) s_cache[i].block = ~0u;
    s_log_misses = getenv("NFL2K5_XBPAK_LOG") != NULL;
    s_load_ms = GetTickCount64();
    s_dctx = ZSTD_createDCtx();
    s_scratch = (uint8_t *)malloc(ZSTD_COMPRESSBOUND(s_hdr.max_ulen));
    fprintf(stderr, "  [XBPAK] %s: %u files, %u blocks, cache %llu MB%s%s\n", path, s_hdr.nfiles, s_hdr.nblocks,
            (unsigned long long)(s_cache_budget >> 20), s_drop_flags & XBPAK_DROP_MOVIES ? ", movies left out" : "",
            s_drop_flags & XBPAK_DROP_CRIB ? ", Crib assets left out" : "");
    return 1;
bad:
    fprintf(stderr, "  [XBPAK] %s: index is corrupt; not used\n", path);
    free(s_pfiles);
    s_pfiles = NULL;
    free(ix);
fail:
    CloseHandle(s_pak);
    s_pak = INVALID_HANDLE_VALUE;
    return 0;
}

int xbox_pack_active(void) { return s_pfiles != NULL; }
unsigned xbox_pack_drop_flags(void) { return s_pfiles ? s_drop_flags : 0; }

/* The packed file a host path names (matched on its tail), or -1. */
static int pak_find(const WCHAR *win_path)
{
    size_t lp;
    uint32_t i;
    if (!s_pfiles || !win_path) return -1;
    lp = w16len(win_path);
    for (i = 0; i < s_hdr.nfiles; i++) {
        size_t lr = w16len(s_pfiles[i].rel);
        size_t k;
        int same = lp > lr && (win_path[lp - lr - 1] == '\\' || win_path[lp - lr - 1] == '/');
        /* "vc_53450030\3" also matches ".../vc_53450030/3" on POSIX hosts */
        for (k = 0; same && k < lr; k++) {
            WCHAR a = win_path[lp - lr + k], b = s_pfiles[i].rel[k];
            if (a == '/') a = '\\';
            if (b == '/') b = '\\';
            if (a != b) {
                WCHAR one[2] = { a, 0 }, two[2] = { b, 0 };
                same = !w16icmp(one, two);
            }
        }
        if (same)
            return (int)i;
    }
    return -1;
}

static int pak_handle(HANDLE h)
{
    int k, r = -1;
    if (!s_pfiles || !h) return -1;
    AcquireSRWLockShared(&s_open_lock);
    for (k = 0; k < PAK_HANDLES; k++)
        if (s_open[k].h == h) { r = (int)s_open[k].file; break; }
    ReleaseSRWLockShared(&s_open_lock);
    return r;
}

HANDLE xbox_pack_open(const WCHAR *win_path)
{
    int fi = pak_find(win_path), k;
    HANDLE h;
    if (fi < 0) return INVALID_HANDLE_VALUE;
    /* A handle of its own (its file position is the packed file's), on the
     * pack itself, so closing and position calls behave. */
    h = CreateFileW(s_pak_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (h == INVALID_HANDLE_VALUE) return h;
    AcquireSRWLockExclusive(&s_open_lock);
    for (k = 0; k < PAK_HANDLES; k++)
        if (!s_open[k].h) { s_open[k].h = h; s_open[k].file = (uint32_t)fi; break; }
    ReleaseSRWLockExclusive(&s_open_lock);
    if (k == PAK_HANDLES) {
        CloseHandle(h);
        fprintf(stderr, "  [XBPAK] too many open packed files\n");
        return INVALID_HANDLE_VALUE;
    }
    {   /* start at 0, as a fresh open does */
        LARGE_INTEGER zero = { 0 };
        SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
    }
    return h;
}

void xbox_pack_close(HANDLE h)
{
    int k;
    if (!s_pfiles) return;
    AcquireSRWLockExclusive(&s_open_lock);
    for (k = 0; k < PAK_HANDLES; k++)
        if (s_open[k].h == h) s_open[k].h = NULL;
    ReleaseSRWLockExclusive(&s_open_lock);
}

/* Copy [off, off+n) of block b into out. Called with s_cache_lock held. */
static int block_copy(uint32_t bi, uint8_t *out, uint32_t n, uint32_t off)
{
    const XbpakBlock *b = &s_blocks[bi];
    CacheSlot *slot = NULL, *lru = NULL;
    int k;
    if (b->method == XBPAK_BLOCK_ZERO) { memset(out, 0, n); return 1; }
    if (b->method == XBPAK_BLOCK_RAW) return pread_all(s_pak, out, n, b->offset + off);
    for (k = 0; k < PAK_SLOTS; k++)
        if (s_cache[k].block == bi) { slot = &s_cache[k]; break; }
    if (slot) {
        InterlockedIncrement64(&s_stat_hits);
    } else {
        size_t r;
        InterlockedIncrement64(&s_stat_misses);
        /* make room: drop least recently used blocks until this one fits */
        for (;;) {
            int free_slot = -1;
            lru = NULL;
            for (k = 0; k < PAK_SLOTS; k++) {
                if (s_cache[k].block == ~0u) { if (free_slot < 0) free_slot = k; continue; }
                if (!lru || s_cache[k].used < lru->used) lru = &s_cache[k];
            }
            if (free_slot >= 0 && s_cache_bytes + b->ulen <= s_cache_budget) { slot = &s_cache[free_slot]; break; }
            if (!lru) { slot = &s_cache[0]; break; }
            s_cache_bytes -= lru->cap;
            free(lru->data);
            lru->data = NULL; lru->cap = 0; lru->block = ~0u;
        }
        if (slot->cap < b->ulen) {
            s_cache_bytes -= slot->cap;
            free(slot->data);
            slot->data = (uint8_t *)malloc(b->ulen);
            slot->cap = slot->data ? b->ulen : 0;
            s_cache_bytes += slot->cap;
            if (!slot->data) return 0;
        }
        slot->block = ~0u;
        {
            LARGE_INTEGER t0, t1, fq;
            QueryPerformanceCounter(&t0);
            if (!pread_all(s_pak, s_scratch, b->csize, b->offset)) return 0;
            r = ZSTD_decompressDCtx(s_dctx, slot->data, b->ulen, s_scratch, b->csize);
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&fq);
            s_stat_us += (t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart;
            if (s_log_misses)
                fprintf(stderr, "  [XBPAK] t=%.1f s block %u (%u KB) unpacked in %.1f ms\n",
                        (double)(GetTickCount64() - s_load_ms) / 1000.0, bi, b->ulen / 1024,
                        (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart);
        }
        if (r != b->ulen) {
            fprintf(stderr, "  [XBPAK] block %u does not decompress (%s)\n", bi,
                    ZSTD_isError(r) ? ZSTD_getErrorName(r) : "short");
            return 0;
        }
        InterlockedAdd64(&s_stat_unpacked, b->ulen);
        slot->block = bi;
        if ((s_stat_misses & 127) == 0) {
            char line[160];
            xbox_pack_stats(line, sizeof line);
            fprintf(stderr, "  [XBPAK] %s\n", line);
        }
    }
    slot->used = ++s_stamp;
    memcpy(out, slot->data + off, n);
    return 1;
}

int xbox_pack_pread(HANDLE h, void *buf, uint32_t len, uint64_t pos, uint32_t *got)
{
    int fi = pak_handle(h);
    const PakFile *f;
    uint64_t end;
    uint32_t lo, hi;
    uint8_t *out = (uint8_t *)buf;
    if (fi < 0) return 0;
    f = &s_pfiles[fi];
    end = pos + len;
    if (end > f->size) end = f->size;
    if (pos >= end) { *got = 0; return 1; }
    /* first block holding pos */
    lo = 0; hi = f->nblocks;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (s_blocks[f->first_block + mid].loff <= pos) lo = mid; else hi = mid;
    }
    AcquireSRWLockExclusive(&s_cache_lock);
    {
        uint64_t p = pos;
        uint32_t bi = f->first_block + lo;
        while (p < end && bi < f->first_block + f->nblocks) {
            const XbpakBlock *b = &s_blocks[bi];
            uint64_t bend = b->loff + b->ulen;
            uint32_t n = (uint32_t)((end < bend ? end : bend) - p);
            if (!block_copy(bi, out + (p - pos), n, (uint32_t)(p - b->loff))) {
                ReleaseSRWLockExclusive(&s_cache_lock);
                memset(out + (p - pos), 0, (size_t)(end - p));
                *got = (uint32_t)(end - pos);
                return 1;
            }
            p += n;
            bi++;
        }
    }
    ReleaseSRWLockExclusive(&s_cache_lock);
    *got = (uint32_t)(end - pos);
    return 1;
}

int xbox_pack_read(HANDLE h, void *buf, ULONG len, const LARGE_INTEGER *off, ULONG *got)
{
    uint64_t pos;
    uint32_t g = 0;
    LARGE_INTEGER newpos;
    if (pak_handle(h) < 0) return 0;
    if (off && off->QuadPart >= 0) {
        pos = (uint64_t)off->QuadPart;
    } else {
        LARGE_INTEGER zero = { 0 }, cur;
        SetFilePointerEx(h, zero, &cur, FILE_CURRENT);
        pos = (uint64_t)cur.QuadPart;
    }
    xbox_pack_pread(h, buf, len, pos, &g);
    newpos.QuadPart = (LONGLONG)(pos + g);
    SetFilePointerEx(h, newpos, NULL, FILE_BEGIN);      /* as a read would leave it */
    *got = g;
    return 1;
}

int xbox_pack_size(HANDLE h, LONGLONG *size)
{
    int fi = pak_handle(h);
    if (fi < 0) return 0;
    *size = (LONGLONG)s_pfiles[fi].size;
    return 1;
}

int xbox_pack_path_size(const WCHAR *win_path, LONGLONG *size)
{
    int fi = pak_find(win_path);
    if (fi < 0) return 0;
    *size = (LONGLONG)s_pfiles[fi].size;
    return 1;
}

void xbox_pack_stats(char *buf, size_t n)
{
    snprintf(buf, n, "cache: %lld hits, %lld misses, %.0f MB unpacked in %.2f s, %.0f MB held",
             (long long)s_stat_hits, (long long)s_stat_misses, s_stat_unpacked / 1048576.0, s_stat_us / 1e6,
             s_cache_bytes / 1048576.0);
}
#else
/* No packs: built without zstd. */
int xbox_pack_load(const char *game_dir) { (void)game_dir; return 0; }
int xbox_pack_active(void) { return 0; }
unsigned xbox_pack_drop_flags(void) { return 0; }
HANDLE xbox_pack_open(const WCHAR *win_path) { (void)win_path; return INVALID_HANDLE_VALUE; }
void xbox_pack_close(HANDLE h) { (void)h; }
int xbox_pack_read(HANDLE h, void *buf, ULONG len, const LARGE_INTEGER *off, ULONG *got)
{ (void)h; (void)buf; (void)len; (void)off; (void)got; return 0; }
int xbox_pack_pread(HANDLE h, void *buf, uint32_t len, uint64_t pos, uint32_t *got)
{ (void)h; (void)buf; (void)len; (void)pos; (void)got; return 0; }
int xbox_pack_size(HANDLE h, LONGLONG *size) { (void)h; (void)size; return 0; }
int xbox_pack_path_size(const WCHAR *win_path, LONGLONG *size) { (void)win_path; (void)size; return 0; }
void xbox_pack_stats(char *buf, size_t n) { if (n) buf[0] = 0; }
#endif
