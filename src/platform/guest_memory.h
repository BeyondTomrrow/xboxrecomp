#ifndef XBOX_GUEST_MEMORY_H
#define XBOX_GUEST_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Checked leaf copies only. A false result may have copied a prefix: callers
 * must discard failed reads and must not assume failed writes are atomic.
 * Zero-sized copies accept NULL buffers but still validate guest/host bounds.
 * Supported implementations: Windows SEH and Linux process_vm_* only. */
bool xbox_guest_memory_read(uintptr_t base, uint64_t address, void *destination, size_t size);
bool xbox_guest_memory_write(uintptr_t base, uint64_t address, const void *source, size_t size);

/* True when [address, address+size) is ordinary guest memory that can be
 * read -- and, with `writable`, written -- in place, with no checked copy.
 * Always false for the device apertures (0xFD000000 and up), whose trapped
 * pages must go through the copies above. Linux: readability is probed with
 * one process_vm_readv per 1024 pages; writability comes from a parsed copy
 * of /proc/self/maps, re-read after xbox_guest_memory_protection_changed().
 * Windows, which reads guest memory under SEH, always answers false. */
bool xbox_guest_memory_direct(uintptr_t base, uint64_t address, size_t size, bool writable);

/* Call after changing page protections (mprotect/munmap/mmap of guest
 * memory) so xbox_guest_memory_direct() re-reads the mappings. */
void xbox_guest_memory_protection_changed(void);
#ifdef __cplusplus
}
#endif
#endif
