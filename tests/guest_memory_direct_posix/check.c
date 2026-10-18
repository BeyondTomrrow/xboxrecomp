/* xbox_guest_memory_direct(): which guest ranges may be used in place.
 * Covers holes, device apertures, read-only and PROT_NONE pages, and that a
 * protection change invalidates earlier answers (the per-generation cache). */
#include "platform/guest_memory.h"
#include <stdio.h>
#include <sys/mman.h>
int main(void) {
    long pg = 4096;
    char *base = mmap(NULL, 64 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(base + 32 * pg, 32 * pg);                                      /* hole after 32 pages */
    int ok = 1;
#define T(c, msg) do { if (!(c)) { ok = 0; printf("FAIL %s\n", msg); } } while (0)
    T(xbox_guest_memory_direct((uintptr_t)base, pg, 8 * pg, 1), "rw range");
    T(!xbox_guest_memory_direct((uintptr_t)base, 30 * pg, 4 * pg, 0), "runs into a hole");
    T(!xbox_guest_memory_direct((uintptr_t)base, 0xFD000100u, 16, 0), "device aperture");
    T(!xbox_guest_memory_direct((uintptr_t)base, 0, 0, 0), "empty");
    mprotect(base + 4 * pg, pg, PROT_READ); xbox_guest_memory_protection_changed();
    T(xbox_guest_memory_direct((uintptr_t)base, 3 * pg, 3 * pg, 0), "read-only page is readable");
    T(!xbox_guest_memory_direct((uintptr_t)base, 3 * pg, 3 * pg, 1), "read-only page is not writable");
    mprotect(base + 4 * pg, pg, PROT_NONE); xbox_guest_memory_protection_changed();
    T(!xbox_guest_memory_direct((uintptr_t)base, 3 * pg, 3 * pg, 0), "PROT_NONE page is not readable");
    mprotect(base + 4 * pg, pg, PROT_READ | PROT_WRITE); xbox_guest_memory_protection_changed();
    T(xbox_guest_memory_direct((uintptr_t)base, 3 * pg, 3 * pg, 1), "writable again");
    T(xbox_guest_memory_direct((uintptr_t)base, 0, 32 * pg - 1, 1), "whole mapping, unaligned end");
    puts(ok ? "direct-check PASS" : "direct-check FAIL"); return !ok; }
