/* Disc overlays: a mod pack's changed disc files served from the retail ones.
 *
 * tools/pack-install.py turns a 2K5 Mod Studio .2k5patch into overlay.bin and
 * lit/<n>.bin. For each changed file the index lists extents, sorted and
 * non-overlapping: literal bytes (from lit/<n>.bin) or a copy from a retail
 * file (the same one or another). Everything else reads the retail file at
 * the same offset, and past the retail end reads zeros, up to the file's new
 * size. The retail disc is never written, and a pack switched off is simply
 * not loaded.
 *
 * kernel_file.c calls in at open (attach), read, size queries and close.
 */
#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t dst;
    uint32_t len;
    uint8_t kind;                        /* 0 literal, 1 copy from a retail file */
    uint16_t srcf;                       /* copy: file index */
    uint64_t src;                        /* literal: offset in lit; copy: offset in srcf */
} OvlExtent;

typedef struct {
    WCHAR rel[96];                       /* "vc_53450030\3", backslashes */
    uint64_t base_size, size;
    int changed;
    OvlExtent *ext;
    uint32_t next;
    HANDLE lit;                          /* lit/<n>.bin, opened on first use */
    HANDLE base;                         /* the retail file, for copies out of it */
} OvlFile;

static OvlFile *s_files;
static uint32_t s_nfiles;
static WCHAR s_dir[MAX_PATH], s_disc[MAX_PATH];
static SRWLOCK s_lock = SRWLOCK_INIT;

#define OVL_HANDLES 256
static struct { HANDLE h; uint32_t file; } s_open[OVL_HANDLES];

static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

/* Load <dir>\overlay.bin; disc is the retail disc folder. Returns the number
 * of changed files, 0 when there is no overlay. */
int xbox_overlay_load(const char *dir, const char *disc)
{
    char path[MAX_PATH];
    FILE *f;
    char magic[8];
    uint32_t nfiles, nchanged, i;
    snprintf(path, sizeof path, "%s\\overlay.bin", dir);
    f = fopen(path, "rb");
    if (!f) return 0;
    if (!rd(f, magic, 8) || memcmp(magic, "2K5OVL1\0", 8) || !rd(f, &nfiles, 4) || !rd(f, &nchanged, 4)
            || nfiles == 0 || nfiles > 4096) {
        fclose(f);
        fprintf(stderr, "  [OVERLAY] %s is not an overlay index\n", path);
        return 0;
    }
    s_files = (OvlFile *)calloc(nfiles, sizeof *s_files);
    s_nfiles = nfiles;
    for (i = 0; i < nfiles; i++) {
        uint16_t n;
        char name[192];
        if (!rd(f, &n, 2) || n >= sizeof name || !rd(f, name, n)) goto bad;
        name[n] = 0;
        for (char *c = name; *c; c++) if (*c == '/') *c = '\\';
        MultiByteToWideChar(CP_UTF8, 0, name, -1, s_files[i].rel, 96);
        if (!rd(f, &s_files[i].base_size, 8) || !rd(f, &s_files[i].size, 8)) goto bad;
    }
    for (i = 0; i < nchanged; i++) {
        uint32_t idx, n, k;
        if (!rd(f, &idx, 4) || !rd(f, &n, 4) || idx >= nfiles) goto bad;
        s_files[idx].changed = 1;
        s_files[idx].next = n;
        s_files[idx].ext = (OvlExtent *)malloc((size_t)n * sizeof(OvlExtent) + 1);
        for (k = 0; k < n; k++) {
            OvlExtent *e = &s_files[idx].ext[k];
            if (!rd(f, &e->dst, 8) || !rd(f, &e->len, 4) || !rd(f, &e->kind, 1) || !rd(f, &e->srcf, 2)
                    || !rd(f, &e->src, 8)) goto bad;
            if (e->kind == 1 && e->srcf >= nfiles) goto bad;
        }
    }
    fclose(f);
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, s_dir, MAX_PATH);
    MultiByteToWideChar(CP_UTF8, 0, disc, -1, s_disc, MAX_PATH);
    fprintf(stderr, "  [OVERLAY] %s: %u of %u disc files changed\n", dir, nchanged, nfiles);
    return (int)nchanged;
bad:
    fclose(f);
    fprintf(stderr, "  [OVERLAY] %s is truncated or corrupt; not used\n", path);
    free(s_files);
    s_files = NULL;
    s_nfiles = 0;
    return 0;
}

/* The changed file a host path names, or -1. Matched on the path's tail, so
 * any spelling of the disc folder works. */
static int ovl_find(const WCHAR *win_path)
{
    size_t lp;
    uint32_t i;
    if (!s_nfiles || !win_path) return -1;
    lp = wcslen(win_path);
    for (i = 0; i < s_nfiles; i++) {
        size_t lr;
        if (!s_files[i].changed) continue;
        lr = wcslen(s_files[i].rel);
        if (lp > lr && win_path[lp - lr - 1] == L'\\' && !_wcsicmp(win_path + lp - lr, s_files[i].rel))
            return (int)i;
    }
    return -1;
}

static int ovl_handle(HANDLE h)
{
    int k;
    if (!s_nfiles || !h) return -1;
    for (k = 0; k < OVL_HANDLES; k++)
        if (s_open[k].h == h) return (int)s_open[k].file;
    return -1;
}

int xbox_overlay_wants(const WCHAR *win_path) { return ovl_find(win_path) >= 0; }

void xbox_overlay_attach(HANDLE h, const WCHAR *win_path)
{
    int fi = ovl_find(win_path), k;
    if (fi < 0) return;
    AcquireSRWLockExclusive(&s_lock);
    for (k = 0; k < OVL_HANDLES; k++)
        if (!s_open[k].h) { s_open[k].h = h; s_open[k].file = (uint32_t)fi; break; }
    ReleaseSRWLockExclusive(&s_lock);
    if (k == OVL_HANDLES)
        fprintf(stderr, "  [OVERLAY] too many open overlay files; %S reads unpatched\n", win_path);
}

void xbox_overlay_detach(HANDLE h)
{
    int k;
    if (!s_nfiles) return;
    AcquireSRWLockExclusive(&s_lock);
    for (k = 0; k < OVL_HANDLES; k++)
        if (s_open[k].h == h) s_open[k].h = NULL;
    ReleaseSRWLockExclusive(&s_lock);
}

static HANDLE ovl_open(const WCHAR *path)
{
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* Positional read of exactly n bytes (zeros past the end). */
static int pread_all(HANDLE h, void *buf, uint32_t n, uint64_t off)
{
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        OVERLAPPED ov;
        DWORD got = 0;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
        if (!ReadFile(h, p, n, &got, &ov) || !got) {
            memset(p, 0, n);
            return GetLastError() == ERROR_HANDLE_EOF;
        }
        p += got; n -= got; off += got;
    }
    return 1;
}

static HANDLE ovl_lit(uint32_t fi)
{
    OvlFile *f = &s_files[fi];
    if (!f->lit) {
        WCHAR p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%s\\lit\\%u.bin", s_dir, fi);
        f->lit = ovl_open(p);
    }
    return f->lit;
}

static HANDLE ovl_base(uint32_t fi)
{
    OvlFile *f = &s_files[fi];
    if (!f->base) {
        WCHAR p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%s\\%s", s_disc, f->rel);
        f->base = ovl_open(p);
    }
    return f->base;
}

/* Read through the overlay. Returns 1 when h is an overlay file (and *got,
 * the bytes read, is set), 0 to read the file normally. off: NULL or
 * negative = the handle's current position. */
int xbox_overlay_read(HANDLE h, void *buf, ULONG len, const LARGE_INTEGER *off, ULONG *got)
{
    int fi = ovl_handle(h);
    OvlFile *f;
    uint64_t pos, end;
    uint32_t lo, hi;
    uint8_t *out = (uint8_t *)buf;
    LARGE_INTEGER newpos;
    if (fi < 0) return 0;
    f = &s_files[fi];
    if (off && off->QuadPart >= 0) {
        pos = (uint64_t)off->QuadPart;
    } else {
        LARGE_INTEGER zero = { 0 }, cur;
        SetFilePointerEx(h, zero, &cur, FILE_CURRENT);
        pos = (uint64_t)cur.QuadPart;
    }
    end = pos + len;
    if (end > f->size) end = f->size;
    if (pos >= end) { *got = 0; return 1; }
    /* Retail bytes underneath, zeros past the retail end. */
    if (pos < f->base_size) {
        uint64_t b = end < f->base_size ? end : f->base_size;
        pread_all(h, out, (uint32_t)(b - pos), pos);
        if (b < end) memset(out + (b - pos), 0, (size_t)(end - b));
    } else {
        memset(out, 0, (size_t)(end - pos));
    }
    /* The extents that intersect [pos, end): binary search for the first. */
    lo = 0; hi = f->next;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (f->ext[mid].dst + f->ext[mid].len <= pos) lo = mid + 1; else hi = mid;
    }
    for (; lo < f->next && f->ext[lo].dst < end; lo++) {
        const OvlExtent *e = &f->ext[lo];
        uint64_t a = e->dst > pos ? e->dst : pos;
        uint64_t b = e->dst + e->len < end ? e->dst + e->len : end;
        HANDLE src = e->kind == 0 ? ovl_lit((uint32_t)fi) : ovl_base(e->srcf);
        if (src && src != INVALID_HANDLE_VALUE)
            pread_all(src, out + (a - pos), (uint32_t)(b - a), e->src + (a - e->dst));
    }
    newpos.QuadPart = (LONGLONG)end;
    SetFilePointerEx(h, newpos, NULL, FILE_BEGIN);      /* as a read would leave it */
    *got = (ULONG)(end - pos);
    return 1;
}

/* The patched size of an open overlay file / of a path, or 0 if not one. */
int xbox_overlay_size(HANDLE h, LONGLONG *size)
{
    int fi = ovl_handle(h);
    if (fi < 0) return 0;
    *size = (LONGLONG)s_files[fi].size;
    return 1;
}

int xbox_overlay_path_size(const WCHAR *win_path, LONGLONG *size)
{
    int fi = ovl_find(win_path);
    if (fi < 0) return 0;
    *size = (LONGLONG)s_files[fi].size;
    return 1;
}
#else
/* POSIX (Linux, Android): the same overlay, keyed by the fd-backed HANDLEs
 * kernel_file.c's POSIX backend makes. Paths are host paths with '/'. Disc
 * files are under 4 GB, so an extent packs into 16 bytes (Android keeps the
 * whole index in memory: millions of extents for a big pack). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <dirent.h>
#include "platform/win32_compat.h"

typedef struct {
    uint32_t dst, len, src;
    uint16_t srcf;                       /* copy: file index */
    uint8_t kind;                        /* 0 literal (src = offset in lit), 1 copy */
} OvlExtent;

typedef struct {
    char rel[192];                       /* "vc_53450030/3" */
    uint64_t base_size, size;
    int changed;
    OvlExtent *ext;
    uint32_t next;
    int lit_fd, base_fd;                 /* -1 until first use */
} OvlFile;

static OvlFile *s_files;
static uint32_t s_nfiles;
static char s_dir[1024], s_disc[1024];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

#define OVL_HANDLES 256
static struct { HANDLE h; uint32_t file; } s_open[OVL_HANDLES];

static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

int xbox_overlay_load(const char *dir, const char *disc)
{
    char path[1100];
    FILE *f;
    char magic[8];
    uint32_t nfiles, nchanged, i;
    uint64_t total = 0;
    snprintf(path, sizeof path, "%s/overlay.bin", dir);
    f = fopen(path, "rb");
    if (!f) return 0;
    if (!rd(f, magic, 8) || memcmp(magic, "2K5OVL1\0", 8) || !rd(f, &nfiles, 4) || !rd(f, &nchanged, 4)
            || nfiles == 0 || nfiles > 4096) {
        fclose(f);
        fprintf(stderr, "  [OVERLAY] %s is not an overlay index\n", path);
        return 0;
    }
    s_files = (OvlFile *)calloc(nfiles, sizeof *s_files);
    if (!s_files) { fclose(f); return 0; }
    s_nfiles = nfiles;
    for (i = 0; i < nfiles; i++) {
        uint16_t n;
        s_files[i].lit_fd = s_files[i].base_fd = -1;
        if (!rd(f, &n, 2) || n >= sizeof s_files[i].rel || !rd(f, s_files[i].rel, n)) goto bad;
        s_files[i].rel[n] = 0;
        for (char *c = s_files[i].rel; *c; c++) if (*c == '\\') *c = '/';
        if (!rd(f, &s_files[i].base_size, 8) || !rd(f, &s_files[i].size, 8)) goto bad;
    }
    for (i = 0; i < nchanged; i++) {
        uint32_t idx, n, k;
        if (!rd(f, &idx, 4) || !rd(f, &n, 4) || idx >= nfiles || s_files[idx].changed) goto bad;
        s_files[idx].changed = 1;
        s_files[idx].next = n;
        s_files[idx].ext = (OvlExtent *)malloc((size_t)n * sizeof(OvlExtent) + 1);
        if (!s_files[idx].ext) goto bad;
        total += n;
        for (k = 0; k < n; k++) {
            OvlExtent *e = &s_files[idx].ext[k];
            uint64_t dst, src;
            uint32_t len;
            uint8_t kind;
            uint16_t srcf;
            if (!rd(f, &dst, 8) || !rd(f, &len, 4) || !rd(f, &kind, 1) || !rd(f, &srcf, 2) || !rd(f, &src, 8))
                goto bad;
            if ((kind == 1 && srcf >= nfiles) || dst + len > 0xFFFFFFFFull || src + len > 0xFFFFFFFFull) goto bad;
            e->dst = (uint32_t)dst; e->len = len; e->src = (uint32_t)src; e->srcf = srcf; e->kind = kind;
        }
    }
    fclose(f);
    snprintf(s_dir, sizeof s_dir, "%s", dir);
    snprintf(s_disc, sizeof s_disc, "%s", disc);
    fprintf(stderr, "  [OVERLAY] %s: %u of %u disc files changed, %llu extents\n", dir, nchanged, nfiles,
            (unsigned long long)total);
    return (int)nchanged;
bad:
    fclose(f);
    fprintf(stderr, "  [OVERLAY] %s is truncated or corrupt; not used\n", path);
    for (i = 0; i < nfiles; i++) free(s_files[i].ext);
    free(s_files);
    s_files = NULL;
    s_nfiles = 0;
    return 0;
}

/* The changed file a host path names, or -1 (matched on the path's tail). */
static int ovl_find(const char *host_path)
{
    size_t lp;
    uint32_t i;
    if (!s_nfiles || !host_path) return -1;
    lp = strlen(host_path);
    for (i = 0; i < s_nfiles; i++) {
        size_t lr;
        if (!s_files[i].changed) continue;
        lr = strlen(s_files[i].rel);
        if (lp > lr && (host_path[lp - lr - 1] == '/' || host_path[lp - lr - 1] == '\\')) {
            const char *a = host_path + lp - lr, *b = s_files[i].rel;
            size_t k;
            for (k = 0; k < lr; k++) {          /* case-blind, '\' == '/' */
                char x = a[k] == '\\' ? '/' : (char)tolower((unsigned char)a[k]);
                if (x != (char)tolower((unsigned char)b[k])) break;
            }
            if (k == lr) return (int)i;
        }
    }
    return -1;
}

static int ovl_handle(HANDLE h)
{
    int k;
    if (!s_nfiles || !h) return -1;
    for (k = 0; k < OVL_HANDLES; k++)
        if (s_open[k].h == h) return (int)s_open[k].file;
    return -1;
}

void xbox_overlay_attach_path(HANDLE h, const char *host_path)
{
    int fi = ovl_find(host_path), k;
    if (fi < 0) return;
    pthread_mutex_lock(&s_lock);
    for (k = 0; k < OVL_HANDLES; k++)
        if (!s_open[k].h) { s_open[k].h = h; s_open[k].file = (uint32_t)fi; break; }
    pthread_mutex_unlock(&s_lock);
    if (k == OVL_HANDLES)
        fprintf(stderr, "  [OVERLAY] too many open overlay files; %s reads unpatched\n", host_path);
}

/* The Windows-shaped entry (src/presentation/portraits.cpp): a UTF-16 path. */
void xbox_overlay_attach(HANDLE h, const WCHAR *path)
{
    char p[1024];
    size_t i;
    if (!s_nfiles || !path) return;
    for (i = 0; path[i] && i + 1 < sizeof p; i++) p[i] = path[i] < 0x80 ? (char)path[i] : '?';
    p[i] = 0;
    xbox_overlay_attach_path(h, p);
}

void xbox_overlay_detach(HANDLE h)
{
    int k;
    if (!s_nfiles) return;
    pthread_mutex_lock(&s_lock);
    for (k = 0; k < OVL_HANDLES; k++)
        if (s_open[k].h == h) s_open[k].h = NULL;
    pthread_mutex_unlock(&s_lock);
}

/* Positional read of exactly n bytes (zeros past the end). */
static void pread_all(int fd, void *buf, uint32_t n, uint64_t off)
{
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        ssize_t got = pread(fd, p, n, (off_t)off);
        if (got <= 0) { memset(p, 0, n); return; }
        p += got; n -= (uint32_t)got; off += (uint64_t)got;
    }
}

/* Open dir/rel matching each component of rel without regard to case. The
 * pack's index names disc files the way Windows saw them ("vc_53450030/c")
 * while an extracted disc can spell them otherwise ("vc_53450030/C"); on
 * Windows the two are one file, on Linux and Android they are not. */
static int ovl_open_nocase(const char *dir, const char *rel)
{
    char cur[1300], part[256];
    snprintf(cur, sizeof cur, "%s", dir);
    while (*rel) {
        size_t n = strcspn(rel, "/");
        DIR *d;
        struct dirent *de;
        int found = 0;
        if (n == 0) { rel++; continue; }
        if (n >= sizeof part) return -1;
        memcpy(part, rel, n);
        part[n] = 0;
        rel += n;
        if (!(d = opendir(cur))) return -1;
        while ((de = readdir(d)) != NULL)
            if (!strcasecmp(de->d_name, part)) {
                size_t l = strlen(cur);
                snprintf(cur + l, sizeof cur - l, "/%s", de->d_name);
                found = 1;
                break;
            }
        closedir(d);
        if (!found) return -1;
    }
    return open(cur, O_RDONLY);
}

/* *slot: -1 not opened yet, -2 cannot be opened (reported once). */
static int ovl_fd(int *slot, const char *a, const char *b)
{
    if (*slot == -1) {
        char p[1300];
        pthread_mutex_lock(&s_lock);
        if (*slot == -1) {
            int fd;
            snprintf(p, sizeof p, "%s/%s", a, b);
            fd = open(p, O_RDONLY);
            if (fd < 0) fd = ovl_open_nocase(a, b);
            if (fd < 0) fprintf(stderr, "  [OVERLAY] cannot open %s (its bytes read as zeros)\n", p);
            *slot = fd < 0 ? -2 : fd;
        }
        pthread_mutex_unlock(&s_lock);
    }
    return *slot;
}

int xbox_overlay_read(HANDLE h, void *buf, ULONG len, const LARGE_INTEGER *off, ULONG *got)
{
    int fi = ovl_handle(h), fd;
    OvlFile *f;
    uint64_t pos, end;
    uint32_t lo, hi;
    uint8_t *out = (uint8_t *)buf;
    if (fi < 0) return 0;
    fd = w32_handle_fd(h);
    if (fd < 0) return 0;
    f = &s_files[fi];
    if (off && off->QuadPart >= 0) pos = (uint64_t)off->QuadPart;
    else pos = (uint64_t)lseek(fd, 0, SEEK_CUR);
    end = pos + len;
    if (end > f->size) end = f->size;
    if (pos >= end) { *got = 0; return 1; }
    if (pos < f->base_size) {
        uint64_t b = end < f->base_size ? end : f->base_size;
        pread_all(fd, out, (uint32_t)(b - pos), pos);
        if (b < end) memset(out + (b - pos), 0, (size_t)(end - b));
    } else {
        memset(out, 0, (size_t)(end - pos));
    }
    lo = 0; hi = f->next;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if ((uint64_t)f->ext[mid].dst + f->ext[mid].len <= pos) lo = mid + 1; else hi = mid;
    }
    for (; lo < f->next && f->ext[lo].dst < end; lo++) {
        const OvlExtent *e = &f->ext[lo];
        uint64_t a = e->dst > pos ? e->dst : pos;
        uint64_t b = (uint64_t)e->dst + e->len < end ? (uint64_t)e->dst + e->len : end;
        int src;
        if (e->kind == 0) {
            char lit[64];
            snprintf(lit, sizeof lit, "lit/%d.bin", fi);
            src = ovl_fd(&f->lit_fd, s_dir, lit);
        } else {
            src = ovl_fd(&s_files[e->srcf].base_fd, s_disc, s_files[e->srcf].rel);
        }
        if (src >= 0) pread_all(src, out + (a - pos), (uint32_t)(b - a), e->src + (a - e->dst));
    }
    lseek(fd, (off_t)end, SEEK_SET);                    /* as a read would leave it */
    *got = (ULONG)(end - pos);
    return 1;
}

int xbox_overlay_size(HANDLE h, LONGLONG *size)
{
    int fi = ovl_handle(h);
    if (fi < 0) return 0;
    *size = (LONGLONG)s_files[fi].size;
    return 1;
}

int xbox_overlay_path_size_a(const char *host_path, LONGLONG *size)
{
    int fi = ovl_find(host_path);
    if (fi < 0) return 0;
    *size = (LONGLONG)s_files[fi].size;
    return 1;
}
#endif /* _WIN32 */
