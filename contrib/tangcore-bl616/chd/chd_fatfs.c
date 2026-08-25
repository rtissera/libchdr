/* license:BSD-3-Clause
 * copyright-holders:Romain Tisserand
 *
 * chd_fatfs.c
 *
 * See chd_fatfs.h. Original code, not derived from any third-party source.
 */

#include "chd_fatfs.h"

#include <stdio.h> /* SEEK_SET / SEEK_CUR / SEEK_END */

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

static int chd_fatfs_fclose(void *argp)
{
	FIL *fil = (FIL *)argp;
	return (f_close(fil) == FR_OK) ? 0 : -1;
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

	err = chd_open_core_file_callbacks(&chd_fatfs_callbacks, fil, CHD_OPEN_READ, NULL, chd);
	if (err != CHDERR_NONE)
		f_close(fil);

	return err;
}
