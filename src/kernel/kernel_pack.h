/* Data packs (.xbpak): the title's data folder from one compressed file.
 * See kernel_pack.c and xbpak_format.h. */
#ifndef KERNEL_PACK_H
#define KERNEL_PACK_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Look for <game_dir>\vc_53450030.xbpak; 1 when one is loaded. */
int xbox_pack_load(const char *game_dir);
int xbox_pack_active(void);
/* What the pack left out (XBPAK_DROP_*), 0 without a pack. */
unsigned xbox_pack_drop_flags(void);

#if defined(_WIN32)
#include <windows.h>
/* A handle reading the packed file a host path names, or INVALID_HANDLE_VALUE. */
HANDLE xbox_pack_open(const WCHAR *win_path);
void xbox_pack_close(HANDLE h);
/* 1 when h is a packed file (and the read was served), 0 otherwise. */
int xbox_pack_read(HANDLE h, void *buf, ULONG len, const LARGE_INTEGER *off, ULONG *got);
int xbox_pack_pread(HANDLE h, void *buf, uint32_t len, uint64_t pos, uint32_t *got);
int xbox_pack_size(HANDLE h, LONGLONG *size);
int xbox_pack_path_size(const WCHAR *win_path, LONGLONG *size);
void xbox_pack_stats(char *buf, size_t n);
#endif

#ifdef __cplusplus
}
#endif
#endif
