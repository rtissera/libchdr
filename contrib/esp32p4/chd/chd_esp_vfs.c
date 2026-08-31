/* license:BSD-3-Clause
 * copyright-holders:Romain Tisserand
 *
 * chd_esp_vfs.c
 *
 * See chd_esp_vfs.h. Original code, not derived from any third-party source.
 */

#include "chd_esp_vfs.h"

static uint64_t chd_esp_vfs_fsize(void *argp)
{
	FILE *f = (FILE *)argp;
	long cur, end;
	uint64_t size;

	cur = ftell(f);
	if (cur < 0)
		return 0;

	if (fseek(f, 0, SEEK_END) != 0)
		return 0;

	end = ftell(f);
	fseek(f, cur, SEEK_SET);

	if (end < 0)
		return 0;

	size = (uint64_t)end;
	return size;
}

static size_t chd_esp_vfs_fread(void *ptr, size_t size, size_t nmemb, void *argp)
{
	FILE *f = (FILE *)argp;
	return fread(ptr, size, nmemb, f);
}

static int chd_esp_vfs_fclose(void *argp)
{
	FILE *f = (FILE *)argp;
	return (fclose(f) == 0) ? 0 : -1;
}

static int chd_esp_vfs_fseek(void *argp, int64_t offset, int whence)
{
	FILE *f = (FILE *)argp;
	return fseek(f, (long)offset, whence);
}

const core_file_callbacks chd_esp_vfs_callbacks = {
	.fsize = chd_esp_vfs_fsize,
	.fread = chd_esp_vfs_fread,
	.fclose = chd_esp_vfs_fclose,
	.fseek = chd_esp_vfs_fseek,
};

chd_error chd_esp_vfs_open(const char *path, chd_file **chd)
{
	chd_error err;
	FILE *f;

	f = fopen(path, "rb");
	if (!f)
		return CHDERR_FILE_NOT_FOUND;

	err = chd_open_core_file_callbacks(&chd_esp_vfs_callbacks, f, CHD_OPEN_READ, NULL, chd);
	if (err != CHDERR_NONE)
		fclose(f);

	return err;
}
