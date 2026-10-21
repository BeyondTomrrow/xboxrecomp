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
#include <string.h>
#else
#error "Checked guest-memory copies require Windows or Linux"
#endif


#if !defined(_WIN32)
/* getpid() is a system call; the process never forks, so ask once. */
static pid_t self_pid(void)
{
    static pid_t pid;
    if (!pid)
        pid = getpid();
    return pid;
}
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
    if (xbox_guest_memory_direct(base, address, size, false)) {
        memcpy(destination, (const void *)host, size);
        return true;
    }
    struct iovec local = {destination,size};
    struct iovec remote = {(void *)host,size};
    ssize_t copied = process_vm_readv(self_pid(),&local,1,&remote,1,0);
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
    if (xbox_guest_memory_direct(base, address, size, true)) {
        memcpy((void *)host, source, size);
        return true;
    }
    struct iovec local = {(void *)source,size};
    struct iovec remote = {(void *)host,size};
    ssize_t copied = process_vm_writev(self_pid(),&local,1,&remote,1,0);
    return copied >= 0 && (size_t)copied == size;
#endif
}

#if defined(__linux__)
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>

bool xbox_guest_memory_direct(uintptr_t base, uint64_t address, size_t size, bool writable);

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
        if (process_vm_readv(self_pid(), local, 1, remote, n, 0) != (ssize_t)n)
            return false;
        start = at;
    }
    return true;
}
#endif

#if defined(__linux__)
/* Pages already found accessible, one entry per guest page below the
 * device apertures: (protection generation << 2) | 1 readable | 2 writable.
 * An entry from an older generation does not count, so VirtualAlloc/Free/
 * Protect invalidate everything at once (xbox_guest_memory_protection_
 * changed) without clearing anything, and a stale store from a thread that
 * raced a protection change cannot validate under the new generation.
 * Replaces a 32-range cache: a stadium frame touches hundreds of textures
 * and buffers, so it kept evicting and re-probing with process_vm_readv
 * (~3% of the command thread with its getpid, 2026-10-21). 4 MB. */
#define DIRECT_PAGES (0xFD000000u >> 12)
static _Atomic uint32_t s_page_ok[DIRECT_PAGES];

static uint32_t page_stamp(unsigned gen)
{
    return (uint32_t)gen << 2;
}

/* Every page of [first, last] known accessible (and writable if asked). */
static bool pages_known(uint32_t first, uint32_t last, bool writable, unsigned gen)
{
    uint32_t p, need = writable ? 3u : 1u, stamp = page_stamp(gen);
    for (p = first; p <= last; p++) {
        uint32_t e = atomic_load_explicit(&s_page_ok[p], memory_order_relaxed);
        if ((e & ~3u) != stamp || (e & need) != need)
            return false;
    }
    return true;
}

static void pages_remember(uint32_t first, uint32_t last, bool writable, unsigned gen)
{
    uint32_t p, stamp = page_stamp(gen);
    for (p = first; p <= last; p++) {
        uint32_t e = atomic_load_explicit(&s_page_ok[p], memory_order_relaxed);
        uint32_t keep = (e & ~3u) == stamp ? (e & 2u) : 0u;    /* writable stays known */
        atomic_store_explicit(&s_page_ok[p], stamp | 1u | keep | (writable ? 2u : 0u),
                              memory_order_relaxed);
    }
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
        uint32_t first = (uint32_t)(address >> 12), last = (uint32_t)((address + size - 1) >> 12);
        if (pages_known(first, last, writable, gen))
            return true;
        if (!range_readable(start, end, page) || (writable && !range_writable(start, end)))
            return false;
        pages_remember(first, last, writable, gen);
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
