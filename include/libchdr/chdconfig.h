#ifndef __CHDCONFIG_H__
#define __CHDCONFIG_H__

/* Configure CHDR features by defining these beforehand. */

#ifndef WANT_RAW_DATA_SECTOR
#define WANT_RAW_DATA_SECTOR    1
#endif

#ifndef WANT_SUBCODE
#define WANT_SUBCODE            1
#endif

#ifndef VERIFY_BLOCK_CRC
#define VERIFY_BLOCK_CRC        1
#endif

/* Trade CPU for RAM on the per-hunk map: instead of fully materializing it
 * at chd_open() (12 bytes/hunk for CHDv5, ~24 bytes/hunk for legacy v1-v4 -
 * scales with total hunk count, independent of codec/hunkbytes choice, and
 * can reach multiple MB on full-size CD/GD-ROM/UMD images), keep only a
 * sparse checkpoint index and re-derive individual entries on demand. For
 * memory-constrained targets. See LOWRAM_MAP_CHECKPOINT_STRIDE to tune the
 * RAM/CPU tradeoff. */
#ifndef LOWRAM_MAP
#define LOWRAM_MAP              0
#endif

#ifndef LOWRAM_MAP_CHECKPOINT_STRIDE
#define LOWRAM_MAP_CHECKPOINT_STRIDE 512
#endif

/* Under LOWRAM_MAP, also replace each huffman_decoder's full 2^maxbits
 * direct-lookup table (e.g. 128 KiB at maxbits=16, as used by AVHuff's
 * Y/Cb/Cr contexts and the CHD huffman codec) with a two-level table: a
 * 2^L1BITS first-level table, escaping to small per-prefix subtables only
 * for the rare codes longer than L1BITS. Huffman assigns short codes to
 * common symbols by construction, so the fast (non-escaping) path is
 * unchanged; only long, rare codes pay an extra indirection. Total RAM
 * drops to a few KiB per decoder regardless of maxbits.
 */
#ifndef LOWRAM_MAP_HUFFMAN_L1BITS
#define LOWRAM_MAP_HUFFMAN_L1BITS 10
#endif

#endif
