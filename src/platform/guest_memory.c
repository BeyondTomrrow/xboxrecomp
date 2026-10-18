#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "guest_memory.h"
#if defined(_WIN32)
#include <windows.h>
#include <string.h>
#elif defined(__linux__)
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#else
#error "Checked guest-memory copies require Windows or Linux"
#endif

static bool copy_bounds(uintptr_t base, uint64_t address, const void *buffer,
                        size_t size, uintptr_t *host)
{
    if (address > UINT32_MAX || (uint64_t)size > UINT64_C(0x100000000) - address ||
        address > UINTPTR_MAX - base)
        return false;
    *host = base + (uintptr_t)address;
    if (size > UINTPTR_MAX - *host || (size && !buffer) ||
        size > UINTPTR_MAX - (uintptr_t)buffer)
        return false;
    return true;
}

bool xbox_guest_memory_read(uintptr_t base, uint64_t address, void *destination, size_t size)
{
    uintptr_t host;
    if (!copy_bounds(base,address,destination,size,&host)) return false;
    if (!size) return true;
#if defined(_WIN32)
    __try { memcpy(destination,(const void *)host,size); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
#else
    struct iovec local = {destination,size};
    struct iovec remote = {(void *)host,size};
    ssize_t copied = process_vm_readv(getpid(),&local,1,&remote,1,0);
    return copied >= 0 && (size_t)copied == size;
#endif
}

bool xbox_guest_memory_write(uintptr_t base, uint64_t address, const void *source, size_t size)
{
    uintptr_t host;
    if (!copy_bounds(base,address,source,size,&host)) return false;
    if (!size) return true;
#if defined(_WIN32)
    __try { memcpy((void *)host,source,size); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
#else
    struct iovec local = {(void *)source,size};
    struct iovec remote = {(void *)host,size};
    ssize_t copied = process_vm_writev(getpid(),&local,1,&remote,1,0);
    return copied >= 0 && (size_t)copied == size;
#endif
}

#if defined(__linux__)
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>

/* ---- writability: a parsed copy of /proc/self/maps ---------------------- */
typedef struct { uintptr_t start, end; bool writable; } GuestMapping;

static atomic_uint s_protection_generation = 1;
static pthread_mutex_t s_maps_lock = PTHREAD_MUTEX_INITIALIZER;
static GuestMapping *s_maps;
static size_t s_maps_count, s_maps_cap;
static unsigned s_maps_generation;          /* 0: never read */

static bool maps_reload(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    if (!f)
        return false;
    s_maps_count = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long start, end;
        char perms[5];
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3)
            continue;
        if (s_maps_count == s_maps_cap) {
            size_t cap = s_maps_cap ? s_maps_cap * 2 : 256;
            GuestMapping *grown = (GuestMapping *)realloc(s_maps, cap * sizeof *grown);
            if (!grown) { fclose(f); return false; }
            s_maps = grown;
            s_maps_cap = cap;
        }
        s_maps[s_maps_count++] = (GuestMapping){ start, end, perms[1] == 'w' };
    }
    fclose(f);
    return true;                             /* /proc/self/maps is sorted by address */
}

/* Is every byte of [start, end) covered by writable mappings? */
static bool maps_writable(uintptr_t start, uintptr_t end)
{
    size_t i;
    for (i = 0; i < s_maps_count && start < end; i++) {
        if (s_maps[i].end <= start)
            continue;
        if (s_maps[i].start > start || !s_maps[i].writable)
            return false;                    /* a hole, or a read-only piece */
        start = s_maps[i].end;
    }
    return start >= end;
}

static bool range_writable(uintptr_t start, uintptr_t end)
{
    unsigned gen = atomic_load(&s_protection_generation);
    bool ok;
    pthread_mutex_lock(&s_maps_lock);
    if (s_maps_generation != gen && maps_reload())
        s_maps_generation = gen;
    ok = s_maps_generation == gen && maps_writable(start, end);
    pthread_mutex_unlock(&s_maps_lock);
    return ok;
}

/* ---- readability: one byte from every page, in as few syscalls as possible */
static bool range_readable(uintptr_t start, uintptr_t end, size_t page)
{
    enum { BATCH = 1024 };                   /* IOV_MAX */
    struct iovec local[1], remote[BATCH];
    unsigned char sink[BATCH];
    while (start < end) {
        size_t n = 0;
        uintptr_t at = start;
        for (; at < end && n < BATCH; at += page)
            remote[n++] = (struct iovec){ (void *)at, 1 };
        local[0] = (struct iovec){ sink, n };
        if (process_vm_readv(getpid(), local, 1, remote, n, 0) != (ssize_t)n)
            return false;
        start = at;
    }
    return true;
}
#endif

#if defined(__linux__)
/* Ranges already found accessible, valid for one protection generation.
 * Render targets and font pages are the same few ranges every frame, so
 * almost every check is a lookup here instead of a page probe. */
#define DIRECT_CACHE 32
typedef struct { uintptr_t start, end; bool writable; unsigned generation; } DirectRange;
static DirectRange s_direct[DIRECT_CACHE];
static unsigned s_direct_next;

static bool direct_cached(uintptr_t start, uintptr_t end, bool writable, unsigned gen)
{
    bool hit = false;
    int i;
    pthread_mutex_lock(&s_maps_lock);
    for (i = 0; i < DIRECT_CACHE && !hit; i++)
        hit = s_direct[i].generation == gen && s_direct[i].start <= start && end <= s_direct[i].end
           && (s_direct[i].writable || !writable);
    pthread_mutex_unlock(&s_maps_lock);
    return hit;
}

static void direct_remember(uintptr_t start, uintptr_t end, bool writable, unsigned gen)
{
    pthread_mutex_lock(&s_maps_lock);
    s_direct[s_direct_next++ % DIRECT_CACHE] = (DirectRange){ start, end, writable, gen };
    pthread_mutex_unlock(&s_maps_lock);
}
#endif

bool xbox_guest_memory_direct(uintptr_t base, uint64_t address, size_t size, bool writable)
{
#if defined(__linux__)
    static size_t page;
    uintptr_t host, start, end;

    if (!size || address >= 0xFD000000u || address + size > 0xFD000000u)
        return false;
    if (!copy_bounds(base, address, (const void *)1, size, &host))
        return false;
    if (!page)
        page = (size_t)sysconf(_SC_PAGESIZE);
    start = host & ~(uintptr_t)(page - 1);
    end = (host + size + page - 1) & ~(uintptr_t)(page - 1);
    {
        unsigned gen = atomic_load(&s_protection_generation);
        if (direct_cached(start, end, writable, gen))
            return true;
        if (!range_readable(start, end, page) || (writable && !range_writable(start, end)))
            return false;
        direct_remember(start, end, writable, gen);
        return true;
    }
#else
    (void)base; (void)address; (void)size; (void)writable;
    return false;
#endif
}

void xbox_guest_memory_protection_changed(void)
{
#if defined(__linux__)
    atomic_fetch_add(&s_protection_generation, 1);
#endif
}
