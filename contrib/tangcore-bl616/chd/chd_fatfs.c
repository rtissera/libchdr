/* license:BSD-3-Clause
 * copyright-holders:Romain Tisserand
 *
 * chd_fatfs.c
 *
 * See chd_fatfs.h. Original code, not derived from any third-party source.
 */

#include "chd_fatfs.h"

#include <stdio.h>  /* SEEK_SET / SEEK_CUR / SEEK_END */
#include <stdlib.h> /* malloc / realloc / free */

static uint64_t chd_fatfs_fsize(void *argp)
{
	FIL *fil = (FIL *)argp;
	return (uint64_t)f_size(fil);
}

static size_t chd_fatfs_fread(void *ptr, size_t size, size_t nmemb, void *argp)
{
	FIL *fil = (FIL *)argp;
	UINT br = 0;
	UINT btr = (UINT)(size * nmemb);

	if (btr == 0)
		return 0;

	if (f_read(fil, ptr, btr, &br) != FR_OK)
		return 0;

	return (size_t)(br / size);
}

/* libchdr calls this once, from chd_close() or from a failed open; the link
 * map goes with the file. */
static int chd_fatfs_fclose(void *argp)
{
	FIL *fil = (FIL *)argp;
	FRESULT fr;
#if FF_USE_FASTSEEK
	DWORD *clmt = fil->cltbl;

	fil->cltbl = NULL;
#endif
	fr = f_close(fil);
#if FF_USE_FASTSEEK
	free(clmt);
#endif
	return (fr == FR_OK) ? 0 : -1;
}

static int chd_fatfs_fseek(void *argp, int64_t offset, int whence)
{
	FIL *fil = (FIL *)argp;
	FSIZE_t abs_offset;

	switch (whence) {
	case SEEK_SET:
		if (offset < 0)
			return -1;
		abs_offset = (FSIZE_t)offset;
		break;
	case SEEK_CUR:
		abs_offset = (FSIZE_t)((int64_t)f_tell(fil) + offset);
		break;
	case SEEK_END:
		abs_offset = (FSIZE_t)((int64_t)f_size(fil) + offset);
		break;
	default:
		return -1;
	}

	return (f_lseek(fil, abs_offset) == FR_OK) ? 0 : -1;
}

#if FF_USE_FASTSEEK
/* Without a cluster link map, every backward f_lseek walks the FAT chain from
 * the file's first cluster again - and every COMPRESSION_SELF hunk is a
 * backward seek, to an earlier hunk the file references instead of storing
 * twice. The cost grows with how far into the file the reader is, so a short
 * test near the start never shows it; on an ESP32-P4 a 29% self-referenced
 * image did not finish in 47 minutes until the map was built. FF_USE_FASTSEEK
 * provides the map, but f_open does not build one - the caller has to.
 *
 * The table needs two words per fragment plus one. A freshly written card
 * holds each file as one fragment, so start small and grow only to what
 * FatFs reports it needs, capped so a pathologically fragmented file costs at
 * most CHD_FATFS_CLMT_MAX words and otherwise just runs without the map. */
#ifndef CHD_FATFS_CLMT_FIRST
#define CHD_FATFS_CLMT_FIRST 16
#endif
#ifndef CHD_FATFS_CLMT_MAX
#define CHD_FATFS_CLMT_MAX 1024
#endif

static void chd_fatfs_map_clusters(FIL *fil)
{
	DWORD *clmt = (DWORD *)malloc(sizeof(DWORD) * CHD_FATFS_CLMT_FIRST);
	FRESULT fr;

	if (clmt == NULL)
		return;

	clmt[0] = CHD_FATFS_CLMT_FIRST;
	fil->cltbl = clmt;
	fr = f_lseek(fil, CREATE_LINKMAP);

	/* on FR_NOT_ENOUGH_CORE, FatFs leaves the size it needs in clmt[0] */
	if (fr == FR_NOT_ENOUGH_CORE && clmt[0] <= CHD_FATFS_CLMT_MAX)
	{
		DWORD need = clmt[0];
		DWORD *grown = (DWORD *)realloc(clmt, sizeof(DWORD) * need);

		if (grown != NULL)
		{
			clmt = grown;
			clmt[0] = need;
			fil->cltbl = clmt;
			fr = f_lseek(fil, CREATE_LINKMAP);
		}
	}

	if (fr != FR_OK)
	{
		/* no map: slower backward seeks, but correct */
		fil->cltbl = NULL;
		free(clmt);
	}

	f_lseek(fil, 0);
}
#endif

const core_file_callbacks chd_fatfs_callbacks = {
	.fsize = chd_fatfs_fsize,
	.fread = chd_fatfs_fread,
	.fclose = chd_fatfs_fclose,
	.fseek = chd_fatfs_fseek,
};

chd_error chd_fatfs_open(const char *path, FIL *fil, chd_file **chd)
{
	chd_error err;

	if (f_open(fil, path, FA_READ) != FR_OK)
		return CHDERR_FILE_NOT_FOUND;

#if FF_USE_FASTSEEK
	/* before chd_open, so the header and map reads benefit too */
	chd_fatfs_map_clusters(fil);
#endif

	/* on failure libchdr has already closed fil through the callback, which
	 * also frees the link map */
	err = chd_open_core_file_callbacks(&chd_fatfs_callbacks, fil, CHD_OPEN_READ, NULL, chd);
	if (err != CHDERR_NONE)
		return err;

	/* Compressed hunks are small - a few KB - and laid out strictly
	 * sequentially, so one larger read serves many of them and the fixed cost
	 * of each f_read (FatFs bookkeeping, SPI command setup, DMA, interrupt)
	 * is paid far less often. Measured 1.11x on an ESP32-S3 and 1.05-1.12x on
	 * an RP2350, both reading over SPI, with 32KB the knee on the RP2350: 64KB
	 * doubled the cost for under 0.7% more.
	 *
	 * Not measured on BL616. The budget is a ceiling, not an allocation
	 * request: an image whose hunks exceed it simply leaves caching off rather
	 * than over-allocating, and a failure here is not fatal to the open. */
	if (CHD_FATFS_CACHE_BUDGET != 0)
		(void)chd_set_cache_budget(*chd, CHD_FATFS_CACHE_BUDGET);

	return CHDERR_NONE;
}
