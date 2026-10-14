/* Cached environment lookups for the APU sources.
 *
 * The voice processor asks for its debug switches (RECOMP_APU_VOICE_LOG
 * and friends) per voice, per 1.33 ms frame. CRT getenv scans the whole
 * environment each time: 17% of the audio thread in a 2026-10-02 profile.
 * The environment does not change while the game runs, so each name is
 * read once, keyed on the literal's address (one or two probes a call).
 * Include after <stdlib.h>; it redirects getenv in the including file. */
#ifndef APU_ENV_H
#define APU_ENV_H

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#include <stdlib.h>
#include <stdint.h>

static const char *apu_env_cached(const char *name)
{
    static struct { const char *volatile name; const char *value; } tab[256];
#ifdef _WIN32
    static SRWLOCK lock = SRWLOCK_INIT;
#else
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#endif
    uintptr_t k = (uintptr_t)name;
    unsigned start = (unsigned)(((k >> 3) * 2654435761u) >> 8) & 255u, h, i;
    for (i = 0, h = start; i < 256; i++, h = (h + 1) & 255u) {
        const char *n = tab[h].name;
        if (n == name) return tab[h].value;
        if (!n) break;
    }
#ifdef _WIN32
    AcquireSRWLockExclusive(&lock);
#else
    pthread_mutex_lock(&lock);
#endif
    for (i = 0, h = start; i < 256; i++, h = (h + 1) & 255u) {
        if (tab[h].name == name) break;
        if (!tab[h].name) {
            tab[h].value = getenv(name);
            __sync_synchronize();
            tab[h].name = name;
            break;
        }
    }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&lock);
#else
    pthread_mutex_unlock(&lock);
#endif
    return i < 256 ? tab[h].value : getenv(name);
}
#define getenv(name) apu_env_cached(name)

#endif
