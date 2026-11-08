/* .xbpak: the title's data folder (vc_53450030\*) in one random-access,
 * block-compressed file (2026-11-01). Made by tools/xbpack/xbpack.c, read by
 * kernel_pack.c in place of the folder's files.
 *
 * Every file is cut into blocks; each block is stored raw, zstd-compressed
 * (independently, so any byte can be read by decompressing one block), or
 * not at all (all zeros). The data repeats itself at distances of several
 * MB, so compressible regions are one big block (block_size, 16 MB by
 * default: about 40% smaller than 256 KB blocks); regions that big blocks
 * do not help (audio, movies) are cut into small ones, cheap to read at
 * random. Files keep their sizes and every offset, so the title's own
 * archive index still works.
 *
 * A pack can leave content out ("dropped"): movies are cut to a short stub
 * that still parses as an MPEG program stream, and the rest of the entry is
 * zeros. The dropped ranges are listed per file and the header flags say
 * which kinds were dropped, so the game can skip what is not there.
 *
 * Layout, little-endian:
 *   XbpakHeader (64 bytes)
 *   block data
 *   index at index_off: for each file
 *       u16 name_len, name (UTF-8, '/' separators, relative to the game
 *       folder, e.g. "vc_53450030/3"), u64 size, u64 xxh64 (of the packed
 *       content), u32 first_block, u32 nblocks, u32 ndropped,
 *       ndropped x { u64 offset, u64 length, u32 kind }
 *   then nblocks_total x XbpakBlock
 */
#ifndef XBPAK_FORMAT_H
#define XBPAK_FORMAT_H
#include <stdint.h>

#define XBPAK_MAGIC "2K5XPAK1"
#define XBPAK_VERSION 1

#define XBPAK_DROP_MOVIES 1u        /* MPEG movies cut to stubs */
#define XBPAK_DROP_CRIB   2u        /* Crib-only assets zeroed */

#define XBPAK_BLOCK_ZERO 0          /* not stored: all zeros */
#define XBPAK_BLOCK_RAW  1
#define XBPAK_BLOCK_ZSTD 2

#pragma pack(push, 1)
typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t block_size;
    uint32_t nfiles;
    uint32_t drop_flags;
    uint64_t index_off;
    uint64_t index_size;
    uint32_t nblocks;
    uint32_t max_ulen;              /* the largest block, for buffers */
    uint8_t reserved[16];
} XbpakHeader;

typedef struct {
    uint64_t offset;                /* in the pack */
    uint64_t loff;                  /* in the file */
    uint32_t csize;                 /* stored bytes */
    uint32_t ulen;                  /* file bytes it holds */
    uint8_t method;                 /* XBPAK_BLOCK_* */
    uint8_t pad[7];
} XbpakBlock;
#pragma pack(pop)

#endif
